# Working rules (ARPG fork)

- **Jeff builds this server with MSVC on Windows.** Windows headers define `near`, `far` and
  `small` as macros (and `min`/`max` unless NOMINMAX), so never use those words as identifiers:
  no variable, field, parameter or function named `near`, `far` or `small`, and never call
  `min`/`max` unqualified (write `std::min`/`std::max`). A GCC build here does not catch it.
- Before every commit touching C++, run the check below and fix every hit:

      git diff HEAD -U0 -- '*.cpp' '*.h' | grep '^+' | grep -wE 'near|far|small'
      git ls-files --others --exclude-standard -- '*.cpp' '*.h' | xargs -r grep -nwE 'near|far|small'

  (the second covers new files). Hits inside comments or strings are fine; an identifier is not.
- More pitfalls: the client repo's `docs/ARPG-HANDOFF.md`, "Pitfalls that have bitten".
- Design decisions and research: the client repo's `docs/ARPG-DECISIONS.md`. Read it before
  planning; add to it when something is decided.
