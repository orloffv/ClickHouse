#include <boost/convert.hpp>
#include <boost/convert/strtol.hpp>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionHelpers.h>
#include <Common/UnorderedMapWithMemoryTracking.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
    extern const int TOO_MANY_ARGUMENTS_FOR_FUNCTION;
    extern const int BAD_ARGUMENTS;
}

namespace
{
    enum class ErrorHandling : uint8_t
    {
        Exception,
        Zero,
        Null
    };

    const UnorderedMapWithMemoryTracking<std::string_view, Float64> time_unit_to_float =
    {
        {"years", 365 * 24 * 3600},
        {"year", 365 * 24 * 3600},
        {"yr", 365 * 24 * 3600},
        {"y", 365 * 24 * 3600},

        {"months", 30.5 * 24 * 3600},
        {"month", 30.5 * 24 * 3600},
        {"mo", 30.5 * 24 * 3600},

        {"weeks", 7 * 24 * 3600},
        {"week", 7 * 24 * 3600},
        {"w", 7 * 24 * 3600},

        {"days", 24 * 3600},
        {"day", 24 * 3600},
        {"d", 24 * 3600},

        {"hours", 3600},
        {"hour", 3600},
        {"hr", 3600},
        {"h", 3600},

        {"minutes", 60},
        {"minute", 60},
        {"min", 60},
        {"m", 60},

        {"seconds", 1},
        {"second", 1},
        {"sec", 1},
        {"s", 1},

        {"milliseconds", 1e-3},
        {"millisecond", 1e-3},
        {"millisec", 1e-3},
        {"ms", 1e-3},

        {"microseconds", 1e-6},
        {"microsecond", 1e-6},
        {"microsec", 1e-6},
        {"μs", 1e-6}, // U+03BC = Greek letter mu
        {"µs", 1e-6}, // U+00B5 = micro symbol
        {"us", 1e-6},

        {"nanoseconds", 1e-9},
        {"nanosecond", 1e-9},
        {"nanosec", 1e-9},
        {"ns", 1e-9},
    };

    /** Prints amount of seconds in form of:
     * "1 year 2 months 4 weeks 12 days 3 hours 1 minute 33 seconds".
     * ' ', ';', '-', '+', ',', ':' can be used as separator, eg. "1yr-2mo", "2m:6s"
     *
     * valid expressions:
     * SELECT parseTimeDelta('1 min 35 sec');
     * SELECT parseTimeDelta('0m;11.23s.');
     * SELECT parseTimeDelta('11hr 25min 3.1s');
     * SELECT parseTimeDelta('0.00123 seconds');
     * SELECT parseTimeDelta('1yr2mo');
     * SELECT parseTimeDelta('11s+22min');
     * SELECT parseTimeDelta('1yr-2mo-4w + 12 days, 3 hours : 1 minute ; 33 seconds');
     *
     * invalid expressions:
     * SELECT parseTimeDelta();
     * SELECT parseTimeDelta('1yr', 1);
     * SELECT parseTimeDelta(1);
     * SELECT parseTimeDelta(' ');
     * SELECT parseTimeDelta('-1yr');
     * SELECT parseTimeDelta('1yr-');
     * SELECT parseTimeDelta('yr2mo');
     * SELECT parseTimeDelta('1.yr2mo');
     * SELECT parseTimeDelta('1-yr');
     * SELECT parseTimeDelta('1 1yr');
     * SELECT parseTimeDelta('1yyr');
     * SELECT parseTimeDelta('1yr-2mo-4w + 12 days, 3 hours : 1 minute ;. 33 seconds');
     *
     * The length of years and months (and even days in presence of time adjustments) are rough:
     * year is just 365 days, month is 30.5 days, day is 86400 seconds, similarly to what formatReadableTimeDelta is doing.
     *
     * parseTimeDeltaOrNull and parseTimeDeltaOrZero return NULL and 0 respectively instead of throwing on an unparseable value.
     */
    class FunctionParseTimeDelta final : public IFunction
    {
    public:
        FunctionParseTimeDelta(const char * name_, ErrorHandling error_handling_)
            : function_name(name_), error_handling(error_handling_) {}

        static FunctionPtr create(ContextPtr, const char * name, ErrorHandling error_handling)
        {
            return std::make_shared<FunctionParseTimeDelta>(name, error_handling);
        }

        String getName() const override { return function_name; }

        bool isVariadic() const override { return true; }

        bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

        size_t getNumberOfArguments() const override { return 0; }

        DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
        {
            if (arguments.empty())
                throw Exception(
                    ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION,
                    "Number of arguments for function {} doesn't match: passed {}, should be 1.",
                    getName(),
                    arguments.size());

            if (arguments.size() > 1)
                throw Exception(
                    ErrorCodes::TOO_MANY_ARGUMENTS_FOR_FUNCTION,
                    "Number of arguments for function {} doesn't match: passed {}, should be 1.",
                    getName(),
                    arguments.size());

            FunctionArgumentDescriptors mandatory_args{
                {"timestr", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), nullptr, "String"},
            };

            validateFunctionArguments(*this, arguments, mandatory_args);

            return getResultType();
        }

        DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override
        {
            return getResultType();
        }

        /// A `LowCardinality` dictionary always holds the type's default value at index 0, even when no
        /// row references it, so a function that throws on the default value must not be executed on
        /// the whole dictionary - it would fail on entirely valid data.
        bool canBeExecutedOnDefaultArguments() const override { return error_handling != ErrorHandling::Exception; }

        bool useDefaultImplementationForConstants() const override { return true; }

        ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
        {
            auto col_to = ColumnFloat64::create(input_rows_count);
            auto & res_data = col_to->getData();

            ColumnUInt8::MutablePtr col_null_map;
            if (error_handling == ErrorHandling::Null)
                col_null_map = ColumnUInt8::create(input_rows_count, false);

            for (size_t i = 0; i < input_rows_count; ++i)
            {
                std::string_view str{arguments[0].column->getDataAt(i)};
                Float64 result = 0;
                if (!parse(str, result))
                {
                    result = 0;
                    if (error_handling == ErrorHandling::Null)
                        col_null_map->getData()[i] = 1;
                }
                res_data[i] = result;
            }

            if (error_handling == ErrorHandling::Null)
                return ColumnNullable::create(std::move(col_to), std::move(col_null_map));
            return col_to;
        }

    private:
        const char * function_name;
        ErrorHandling error_handling;

        DataTypePtr getResultType() const
        {
            DataTypePtr type = std::make_shared<DataTypeFloat64>();
            if (error_handling == ErrorHandling::Null)
                return std::make_shared<DataTypeNullable>(type);
            return type;
        }

        /// Parses one value into `result`. On an unparseable value throws if `error_handling` is `Exception`
        /// and returns false otherwise, so the non-throwing variants don't construct an exception per bad row.
        bool parse(std::string_view str, Float64 & result) const
        {
            Int64 token_tail = 0;
            Int64 token_front = 0;
            Int64 last_pos = str.length() - 1;
            result = 0;

            /// ignore '.' and ' ' at the end of string
            while (last_pos >= 0 && (str[last_pos] == ' ' || str[last_pos] == '.'))
                --last_pos;

            /// no valid characters
            if (last_pos < 0)
            {
                if (error_handling != ErrorHandling::Exception)
                    return false;
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Invalid argument of function {}, no valid characters, str: \"{}\".",
                    getName(),
                    String(str));
            }

            /// last pos character must be character and not be separator or number after ignoring '.' and ' '
            if (!isalpha(str[last_pos]))
            {
                if (error_handling != ErrorHandling::Exception)
                    return false;
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid argument of function {}, str: \"{}\".", getName(), String(str));
            }

            /// scan spaces at the beginning
            scanSpaces(str, token_tail, last_pos);
            token_front = token_tail;

            while (token_tail <= last_pos)
            {
                /// scan unsigned integer
                if (!scanUnsignedInteger(str, token_tail, last_pos))
                {
                    if (error_handling != ErrorHandling::Exception)
                        return false;
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "Invalid argument of function {}, number not found, str: \"{}\".",
                        getName(),
                        String(str));
                }

                /// if there is a '.', then scan another integer to get a float number
                if (token_tail <= last_pos && str[token_tail] == '.')
                {
                    token_tail++;
                    if (!scanUnsignedInteger(str, token_tail, last_pos))
                    {
                        if (error_handling != ErrorHandling::Exception)
                            return false;
                        throw Exception(
                            ErrorCodes::BAD_ARGUMENTS,
                            "Invalid argument of function {}, number not found after '.', str: \"{}\".",
                            getName(),
                            String(str));
                    }
                }

                /// convert float/integer string to float
                Float64 base = 0;
                std::string_view base_str = str.substr(token_front, token_tail - token_front);
                auto value = boost::convert<Float64>(base_str, boost::cnv::strtol());
                if (!value.has_value())
                {
                    if (error_handling != ErrorHandling::Exception)
                        return false;
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "Invalid argument of function {}, can't convert String to Float64: \"{}\".",
                        getName(),
                        String(base_str));
                }
                base = value.get();

                scanSpaces(str, token_tail, last_pos);
                token_front = token_tail;

                /// scan a unit
                if (!scanUnit(str, token_tail, last_pos))
                {
                    if (error_handling != ErrorHandling::Exception)
                        return false;
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "Invalid argument of function {}, time unit not found, str: \"{}\".",
                        getName(),
                        String(str));
                }

                /// get unit number
                std::string_view unit = str.substr(token_front, token_tail - token_front);
                auto iter = time_unit_to_float.find(unit);
                if (iter == time_unit_to_float.end()) /// not find unit
                {
                    if (error_handling != ErrorHandling::Exception)
                        return false;
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS, "Invalid argument of function {}, can't parse the unit: \"{}\".", getName(), unit);
                }
                result += base * iter->second;

                /// scan separator between two tokens
                scanSeparator(str, token_tail, last_pos);
                token_front = token_tail;
            }

            return true;
        }

        /// scan an unsigned integer number
        static bool scanUnsignedInteger(std::string_view & str, Int64 & index, Int64 last_pos)
        {
            int64_t begin_index = index;
            while (index <= last_pos && isdigit(str[index]))
            {
                index++;
            }
            return index != begin_index;
        }

        /// scan a unit
        static bool scanUnit(std::string_view & str, Int64 & index, Int64 last_pos)
        {
            int64_t begin_index = index;
            while (index <= last_pos && !isdigit(str[index]) && !isSeparator(str[index]))
            {
                index++;
            }
            return index != begin_index;
        }

        /// scan spaces
        static void scanSpaces(std::string_view & str, Int64 & index, Int64 last_pos)
        {
            while (index <= last_pos && (str[index] == ' '))
            {
                index++;
            }
        }

        /// scan for characters to ignore
        static void scanSeparator(std::string_view & str, Int64 & index, Int64 last_pos)
        {
            /// ignore spaces
            scanSpaces(str, index, last_pos);

            /// ignore separator
            if (index <= last_pos && isSeparator(str[index]))
            {
                index++;
            }

            scanSpaces(str, index, last_pos);
        }

        static bool isSeparator(char symbol)
        {
            return symbol == ';' || symbol == '-' || symbol == '+' || symbol == ',' || symbol == ':' || symbol == ' ';
        }
    };

}

REGISTER_FUNCTION(ParseTimeDelta)
{
    const String time_units_description = R"(
The time delta string uses these time unit specifications:
- `years`, `year`, `yr`, `y`
- `months`, `month`, `mo`
- `weeks`, `week`, `w`
- `days`, `day`, `d`
- `hours`, `hour`, `hr`, `h`
- `minutes`, `minute`, `min`, `m`
- `seconds`, `second`, `sec`, `s`
- `milliseconds`, `millisecond`, `millisec`, `ms`
- `microseconds`, `microsecond`, `microsec`, `μs`, `µs`, `us`
- `nanoseconds`, `nanosecond`, `nanosec`, `ns`

Multiple time units can be combined with separators (space, `;`, `-`, `+`, `,`, `:`).

The length of years and months are approximations: year is 365 days, month is 30.5 days.
)";
    FunctionDocumentation::Arguments arguments = {
        {"timestr", "A sequence of numbers followed by something resembling a time unit.", {"String"}}
    };
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Other;

    FunctionDocumentation::Description description = R"(
Parse a sequence of numbers followed by something resembling a time unit.
)" + time_units_description + R"(
If the input value cannot be parsed, an exception is thrown. See [`parseTimeDeltaOrNull`](#parseTimeDeltaOrNull) and [`parseTimeDeltaOrZero`](#parseTimeDeltaOrZero) for variants that return `NULL` or `0` instead.
)";
    FunctionDocumentation::Syntax syntax = "parseTimeDelta(timestr)";
    FunctionDocumentation::ReturnedValue returned_value = {"The number of seconds.", {"Float64"}};
    FunctionDocumentation::Examples examples = {
        {
            "Usage example",
            R"(
SELECT parseTimeDelta('11s+22min')
            )",
            R"(
┌─parseTimeDelta('11s+22min')─┐
│                        1331 │
└─────────────────────────────┘
            )"
        },
        {
            "Complex time units",
            R"(
SELECT parseTimeDelta('1yr2mo')
            )",
            R"(
┌─parseTimeDelta('1yr2mo')─┐
│                 36806400 │
└──────────────────────────┘
            )"
        }
    };
    FunctionDocumentation::IntroducedIn introduced_in = {22, 7};
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    FunctionDocumentation::Description description_or_null = R"(
Parse a sequence of numbers followed by something resembling a time unit.
If the input value cannot be parsed, returns `NULL`.
)" + time_units_description + R"(
See also [`parseTimeDelta`](#parseTimeDelta) and [`parseTimeDeltaOrZero`](#parseTimeDeltaOrZero).
)";
    FunctionDocumentation::Syntax syntax_or_null = "parseTimeDeltaOrNull(timestr)";
    FunctionDocumentation::ReturnedValue returned_value_or_null = {"The number of seconds, or `NULL` if the input cannot be parsed.", {"Nullable(Float64)"}};
    FunctionDocumentation::Examples examples_or_null = {
        {
            "Usage example",
            R"(
SELECT arrayJoin(['11s+22min', '1h 30m', '', '10']) AS s, parseTimeDeltaOrNull(s) AS seconds
            )",
            R"(
┌─s─────────┬─seconds─┐
│ 11s+22min │    1331 │
│ 1h 30m    │    5400 │
│           │    ᴺᵁᴸᴸ │
│ 10        │    ᴺᵁᴸᴸ │
└───────────┴─────────┘
            )"
        }
    };
    FunctionDocumentation::IntroducedIn introduced_in_or_null = {26, 10};
    FunctionDocumentation documentation_or_null = {description_or_null, syntax_or_null, arguments, {}, returned_value_or_null, examples_or_null, introduced_in_or_null, category};

    FunctionDocumentation::Description description_or_zero = R"(
Parse a sequence of numbers followed by something resembling a time unit.
If the input value cannot be parsed, returns `0`.
)" + time_units_description + R"(
See also [`parseTimeDelta`](#parseTimeDelta) and [`parseTimeDeltaOrNull`](#parseTimeDeltaOrNull).
)";
    FunctionDocumentation::Syntax syntax_or_zero = "parseTimeDeltaOrZero(timestr)";
    FunctionDocumentation::ReturnedValue returned_value_or_zero = {"The number of seconds, or `0` if the input cannot be parsed.", {"Float64"}};
    FunctionDocumentation::Examples examples_or_zero = {
        {
            "Usage example",
            R"(
SELECT arrayJoin(['11s+22min', '1h 30m', '', '10']) AS s, parseTimeDeltaOrZero(s) AS seconds
            )",
            R"(
┌─s─────────┬─seconds─┐
│ 11s+22min │    1331 │
│ 1h 30m    │    5400 │
│           │       0 │
│ 10        │       0 │
└───────────┴─────────┘
            )"
        }
    };
    FunctionDocumentation::IntroducedIn introduced_in_or_zero = {26, 10};
    FunctionDocumentation documentation_or_zero = {description_or_zero, syntax_or_zero, arguments, {}, returned_value_or_zero, examples_or_zero, introduced_in_or_zero, category};

    factory.registerFunction("parseTimeDelta",
        [](ContextPtr context){ return FunctionParseTimeDelta::create(context, "parseTimeDelta", ErrorHandling::Exception); }, documentation);
    factory.registerFunction("parseTimeDeltaOrNull",
        [](ContextPtr context){ return FunctionParseTimeDelta::create(context, "parseTimeDeltaOrNull", ErrorHandling::Null); }, documentation_or_null);
    factory.registerFunction("parseTimeDeltaOrZero",
        [](ContextPtr context){ return FunctionParseTimeDelta::create(context, "parseTimeDeltaOrZero", ErrorHandling::Zero); }, documentation_or_zero);
}

}
