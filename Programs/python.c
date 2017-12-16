/* Minimal main program -- everything is loaded from the library */

#include "Python.h"
#include "internal/pystate.h"
#include <locale.h>

#ifdef __FreeBSD__
#include <fenv.h>
#endif

#ifdef MS_WINDOWS
int
wmain(int argc, wchar_t **argv)
{
    return Py_Main(argc, argv);
}
#else


int
main(int argc, char **argv)
{
    return _Py_UnixMain(argc, argv);
}
#endif
