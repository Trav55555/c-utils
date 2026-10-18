# JSONTestSuite parsing corpus

The `test_parsing` files of [JSONTestSuite](https://github.com/nst/JSONTestSuite)
by Nicolas Seriot, MIT licensed (see LICENSE), copied unchanged from commit
`1ef36fa`.

`tests/test_json.c` requires every `y_` file to parse and every `n_` file to
fail. An `i_` file may do either, but must not crash. The two
`y_object_duplicated_key*` files are the exception: they must fail, because
`cu_json_parse` refuses repeated keys on purpose.
