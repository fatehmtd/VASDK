#pragma once

#ifdef VASDK_ENABLE_CTRACK
#  include <ctrack.hpp>
#  include <fstream>
#  include <string>
#  define VASDK_CTRACK              CTRACK
#  define VASDK_CTRACK_NAME(n)      CTRACK_NAME(n)
#  define VASDK_CTRACK_PRINT()      ctrack::result_print()
// Write plain-text (no ANSI colours) profiling results to the given file path.
// Uses result_as_string() which consumes the store independently.
#  define VASDK_CTRACK_SAVE(path) \
    do { \
        std::ofstream _ctrack_f(path); \
        if (_ctrack_f.is_open()) { \
            _ctrack_f << ctrack::result_as_string(); \
        } \
    } while(0)
// Drain the store once, render with colours to stdout, render plain-text to file.
#  define VASDK_CTRACK_PRINT_AND_SAVE(path) \
    do { \
        auto _ctrack_res = ctrack::calc_stats_and_clear(); \
        std::cout << "Details\n"; \
        _ctrack_res.get_detail_table(std::cout, true); \
        std::cout << "Summary\n"; \
        _ctrack_res.get_summary_table(std::cout, true); \
        std::ofstream _ctrack_fs(path); \
        if (_ctrack_fs.is_open()) { \
            _ctrack_fs << "Summary\n"; \
            _ctrack_res.get_summary_table(_ctrack_fs, false); \
            _ctrack_fs << "Details\n"; \
            _ctrack_res.get_detail_table(_ctrack_fs, false, true); \
        } \
    } while(0)
#else
#  define VASDK_CTRACK
#  define VASDK_CTRACK_NAME(n)
#  define VASDK_CTRACK_PRINT()
#  define VASDK_CTRACK_SAVE(path)
#  define VASDK_CTRACK_PRINT_AND_SAVE(path)
#endif
