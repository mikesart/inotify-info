/*
 * Copyright 2021 Michael Sartain
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define RESET    "\x1b[0m"
#define GRAY     "\x1b[0;30m"
#define RED      "\x1b[0;31m"
#define GREEN    "\x1b[0;32m"
#define YELLOW   "\x1b[0;33m"
#define BLUE     "\x1b[0;34m"
#define MAGENTA  "\x1b[0;35m"
#define CYAN     "\x1b[0;36m"
#define WHITE    "\x1b[0;37m"
#define BGRAY    "\x1b[1;30m"
#define BRED     "\x1b[1;31m"
#define BGREEN   "\x1b[1;32m"
#define BYELLOW  "\x1b[1;33m"
#define BBLUE    "\x1b[1;34m"
#define BMAGENTA "\x1b[1;35m"
#define BCYAN    "\x1b[1;36m"
#define BWHITE   "\x1b[1;37m"

#define TO_STR( x ) #x
#define TO_STR_VALUE( x ) TO_STR( x )

#define ATTRIBUTE_PRINTF( _x, _y ) __attribute__( ( __format__( __printf__, _x, _y ) ) )

#define GCC_DIAG_STR( s ) #s
#define GCC_DIAG_JOINSTR( x, y ) GCC_DIAG_STR( x##y )
#define GCC_DIAG_DO_PRAGMA( x ) _Pragma( #x )
#define GCC_DIAG_PRAGMA( x ) GCC_DIAG_DO_PRAGMA( GCC diagnostic x )

#define GCC_DIAG_PUSH_OFF( x ) \
    GCC_DIAG_PRAGMA( push )    \
    GCC_DIAG_PRAGMA( ignored GCC_DIAG_JOINSTR( -W, x ) )
#define GCC_DIAG_POP() GCC_DIAG_PRAGMA( pop )

std::string string_formatv(const char *fmt, va_list ap) ATTRIBUTE_PRINTF(1, 0);
std::string string_format(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);

