/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * Default-visibility wrappers for the public headers. The library is built with
 * -fvisibility=hidden so internals stay out of the dynamic symbol table.
 */

#ifndef BARESCRIPT_EXPORT_H
#define BARESCRIPT_EXPORT_H

#if defined(__GNUC__) || defined(__clang__)
#define BS_VISIBILITY_BEGIN _Pragma("GCC visibility push(default)")
#define BS_VISIBILITY_END   _Pragma("GCC visibility pop")
#else
#define BS_VISIBILITY_BEGIN
#define BS_VISIBILITY_END
#endif

#endif
