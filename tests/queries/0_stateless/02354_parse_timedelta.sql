SELECT parseTimeDelta('1 min 35 sec');
SELECT parseTimeDelta('0m;11.23s.');
SELECT parseTimeDelta('11hr 25min 3.1s');
SELECT parseTimeDelta('0.00123 seconds');
SELECT parseTimeDelta('1yr2mo');
SELECT parseTimeDelta('11s+22min');
SELECT parseTimeDelta('1yr-2mo-4w + 12 days, 3 hours : 1 minute ; 33 seconds');
SELECT parseTimeDelta('1s1ms1us1ns');
SELECT parseTimeDelta('1s1ms1μs1ns'); // μs U+03BC
SELECT parseTimeDelta('1s1ms1µs1ns'); // µs U+00B5
SELECT parseTimeDelta('1s - 1ms : 1μs ; 1ns');
SELECT parseTimeDelta('1.11s1.11ms1.11us1.11ns');

-- invalid expressions
SELECT parseTimeDelta(); -- {serverError TOO_FEW_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDelta('1yr', 1); -- {serverError TOO_MANY_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDelta(1); -- {serverError ILLEGAL_TYPE_OF_ARGUMENT}
SELECT parseTimeDelta(' '); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('-1yr'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1yr-'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('yr2mo'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1.yr2mo'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1-yr'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1 1yr'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1yyr'); -- {serverError BAD_ARGUMENTS}
SELECT parseTimeDelta('1yr-2mo-4w + 12 days, 3 hours : 1 minute ;. 33 seconds'); -- {serverError BAD_ARGUMENTS}

-- non-throwing variants
SELECT toTypeName(parseTimeDeltaOrNull('1s')), toTypeName(parseTimeDeltaOrZero('1s'));
SELECT parseTimeDeltaOrNull('11s+22min'), parseTimeDeltaOrZero('11s+22min');

-- valid values give the same result as parseTimeDelta
SELECT count(), countIf(parseTimeDeltaOrNull(s) = parseTimeDelta(s) AND parseTimeDeltaOrZero(s) = parseTimeDelta(s))
FROM values('s String', ('1 min 35 sec'), ('0m;11.23s.'), ('11hr 25min 3.1s'), ('0.00123 seconds'), ('1yr2mo'), ('11s+22min'),
    ('1yr-2mo-4w + 12 days, 3 hours : 1 minute ; 33 seconds'), ('1s1ms1us1ns'), ('1s1ms1μs1ns'), ('1s1ms1µs1ns'), ('1.11s1.11ms1.11us1.11ns'));

-- invalid values give NULL / 0 instead of an exception
SELECT s, parseTimeDeltaOrNull(s), parseTimeDeltaOrZero(s)
FROM values('s String', (''), (' '), ('10'), ('-1yr'), ('1yr-'), ('yr2mo'), ('1.yr2mo'), ('1-yr'), ('1 1yr'), ('1yyr'),
    ('1yr-2mo-4w + 12 days, 3 hours : 1 minute ;. 33 seconds'));

-- a bad row doesn't abort the query
SELECT s, parseTimeDeltaOrNull(s), parseTimeDeltaOrZero(s) FROM values('s String', ('11s+22min'), ('1h 30m'), (''));

-- NULL input
SELECT s, parseTimeDeltaOrNull(s), parseTimeDeltaOrZero(s) FROM values('s Nullable(String)', ('1s'), (NULL), (''));

-- LowCardinality input
SELECT parseTimeDeltaOrNull(materialize(toLowCardinality('1 min'))), parseTimeDeltaOrZero(materialize(toLowCardinality('1 min')));
SELECT parseTimeDeltaOrNull(materialize(toLowCardinality(''))), parseTimeDeltaOrZero(materialize(toLowCardinality('')));

-- errors about the call itself are still raised
SELECT parseTimeDeltaOrNull(); -- {serverError TOO_FEW_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDeltaOrNull('1yr', 1); -- {serverError TOO_MANY_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDeltaOrNull(1); -- {serverError ILLEGAL_TYPE_OF_ARGUMENT}
SELECT parseTimeDeltaOrZero(); -- {serverError TOO_FEW_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDeltaOrZero('1yr', 1); -- {serverError TOO_MANY_ARGUMENTS_FOR_FUNCTION}
SELECT parseTimeDeltaOrZero(1); -- {serverError ILLEGAL_TYPE_OF_ARGUMENT}
