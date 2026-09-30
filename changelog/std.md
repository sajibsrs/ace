# Standard library

- Renamed the internal `std/@` modules after their contents: `rendering` to
  `formatting` (the compiler finds the printf piece writers by that name),
  `float_parse` to `float_parsing`, `float_parse_fast[_tables]` to
  `eisel_lemire[_table]`, and `float_shortest[_tables]` to `ryu[_table]`.
