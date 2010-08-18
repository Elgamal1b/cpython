/* Python interpreter main program */

#include "Python.h"
#include "osdefs.h"
#include "import.h"

#include <locale.h>

#ifdef __VMS
#include <unixlib.h>
#endif

#if defined(MS_WINDOWS) || defined(__CYGWIN__)
#include <windows.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#define PATH_MAX MAXPATHLEN
#endif
#endif

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#if (defined(PYOS_OS2) && !defined(PYCC_GCC)) || defined(MS_WINDOWS)
#define PYTHONHOMEHELP "<prefix>\\lib"
#else
#if defined(PYOS_OS2) && defined(PYCC_GCC)
#define PYTHONHOMEHELP "<prefix>/Lib"
#else
#define PYTHONHOMEHELP "<prefix>/pythonX.X"
#endif
#endif

#include "pygetopt.h"

#define COPYRIGHT \
    "Type \"help\", \"copyright\", \"credits\" or \"license\" " \
    "for more information."

#ifdef __cplusplus
extern "C" {
#endif

/* For Py_GetArgcArgv(); set by main() */
static wchar_t **orig_argv;
static int  orig_argc;

/* command line options */
#define BASE_OPTS L"bBc:dEhiJm:OsStuvVW:xX?"

#define PROGRAM_OPTS BASE_OPTS

/* Short usage message (with %s for argv0) */
static char *usage_line =
"usage: %ls [option] ... [-c cmd | -m mod | file | -] [arg] ...\n";

/* Long usage message, split into parts < 512 bytes */
static char *usage_1 = "\
Options and arguments (and corresponding environment variables):\n\
-b     : issue warnings about str(bytes_instance), str(bytearray_instance)\n\
         and comparing bytes/bytearray with str. (-bb: issue errors)\n\
-B     : don't write .py[co] files on import; also PYTHONDONTWRITEBYTECODE=x\n\
-c cmd : program passed in as string (terminates option list)\n\
-d     : debug output from parser; also PYTHONDEBUG=x\n\
-E     : ignore PYTHON* environment variables (such as PYTHONPATH)\n\
-h     : print this help message and exit (also --help)\n\
";
static char *usage_2 = "\
-i     : inspect interactively after running script; forces a prompt even\n\
         if stdin does not appear to be a terminal; also PYTHONINSPECT=x\n\
-m mod : run library module as a script (terminates option list)\n\
-O     : optimize generated bytecode slightly; also PYTHONOPTIMIZE=x\n\
-OO    : remove doc-strings in addition to the -O optimizations\n\
-s     : don't add user site directory to sys.path; also PYTHONNOUSERSITE\n\
-S     : don't imply 'import site' on initialization\n\
";
static char *usage_3 = "\
-u     : unbuffered binary stdout and stderr; also PYTHONUNBUFFERED=x\n\
         see man page for details on internal buffering relating to '-u'\n\
-v     : verbose (trace import statements); also PYTHONVERBOSE=x\n\
         can be supplied multiple times to increase verbosity\n\
-V     : print the Python version number and exit (also --version)\n\
-W arg : warning control; arg is action:message:category:module:lineno\n\
         also PYTHONWARNINGS=arg\n\
-x     : skip first line of source, allowing use of non-Unix forms of #!cmd\n\
";
static char *usage_4 = "\
file   : program read from script file\n\
-      : program read from stdin (default; interactive mode if a tty)\n\
arg ...: arguments passed to program in sys.argv[1:]\n\n\
Other environment variables:\n\
PYTHONSTARTUP: file executed on interactive startup (no default)\n\
PYTHONPATH   : '%c'-separated list of directories prefixed to the\n\
               default module search path.  The result is sys.path.\n\
";
static char *usage_5 = "\
PYTHONHOME   : alternate <prefix> directory (or <prefix>%c<exec_prefix>).\n\
               The default module search path uses %s.\n\
PYTHONCASEOK : ignore case in 'import' statements (Windows).\n\
PYTHONIOENCODING: Encoding[:errors] used for stdin/stdout/stderr.\n\
PYTHONFSENCODING: Encoding used for the filesystem.\n\
";

FILE *
_Py_wfopen(const wchar_t *path, const wchar_t *mode)
{
#ifndef MS_WINDOWS
    FILE *f;
    char *cpath;
    char cmode[10];
    size_t r;
    r = wcstombs(cmode, mode, 10);
    if (r == (size_t)-1 || r >= 10) {
        errno = EINVAL;
        return NULL;
    }
    cpath = _Py_wchar2char(path);
    if (cpath == NULL)
        return NULL;
    f = fopen(cpath, cmode);
    PyMem_Free(cpath);
    return f;
#else
    return _wfopen(path, mode);
#endif
}


static int
usage(int exitcode, wchar_t* program)
{
    FILE *f = exitcode ? stderr : stdout;

    fprintf(f, usage_line, program);
    if (exitcode)
        fprintf(f, "Try `python -h' for more information.\n");
    else {
        fputs(usage_1, f);
        fputs(usage_2, f);
        fputs(usage_3, f);
        fprintf(f, usage_4, DELIM);
        fprintf(f, usage_5, DELIM, PYTHONHOMEHELP);
    }
#if defined(__VMS)
    if (exitcode == 0) {
        /* suppress 'error' message */
        return 1;
    }
    else {
        /* STS$M_INHIB_MSG + SS$_ABORT */
        return 0x1000002c;
    }
#else
    return exitcode;
#endif
    /*NOTREACHED*/
}

static void RunStartupFile(PyCompilerFlags *cf)
{
    char *startup = Py_GETENV("PYTHONSTARTUP");
    if (startup != NULL && startup[0] != '\0') {
        FILE *fp = fopen(startup, "r");
        if (fp != NULL) {
            (void) PyRun_SimpleFileExFlags(fp, startup, 0, cf);
            PyErr_Clear();
            fclose(fp);
        } else {
            int save_errno;

            save_errno = errno;
            PySys_WriteStderr("Could not open PYTHONSTARTUP\n");
            errno = save_errno;
            PyErr_SetFromErrnoWithFilename(PyExc_IOError,
                            startup);
            PyErr_Print();
            PyErr_Clear();
        }
    }
}


static int RunModule(wchar_t *modname, int set_argv0)
{
    PyObject *module, *runpy, *runmodule, *runargs, *result;
    runpy = PyImport_ImportModule("runpy");
    if (runpy == NULL) {
        fprintf(stderr, "Could not import runpy module\n");
        return -1;
    }
    runmodule = PyObject_GetAttrString(runpy, "_run_module_as_main");
    if (runmodule == NULL) {
        fprintf(stderr, "Could not access runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        return -1;
    }
    module = PyUnicode_FromWideChar(modname, wcslen(modname));
    if (module == NULL) {
        fprintf(stderr, "Could not convert module name to unicode\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        return -1;
    }
    runargs = Py_BuildValue("(Oi)", module, set_argv0);
    if (runargs == NULL) {
        fprintf(stderr,
            "Could not create arguments for runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        Py_DECREF(module);
        return -1;
    }
    result = PyObject_Call(runmodule, runargs, NULL);
    if (result == NULL) {
        PyErr_Print();
    }
    Py_DECREF(runpy);
    Py_DECREF(runmodule);
    Py_DECREF(module);
    Py_DECREF(runargs);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static int RunMainFromImporter(wchar_t *filename)
{
    PyObject *argv0 = NULL, *importer = NULL;

    if ((argv0 = PyUnicode_FromWideChar(filename,wcslen(filename))) &&
        (importer = PyImport_GetImporter(argv0)) &&
        (importer->ob_type != &PyNullImporter_Type))
    {
             /* argv0 is usable as an import source, so
                    put it in sys.path[0] and import __main__ */
        PyObject *sys_path = NULL;
        if ((sys_path = PySys_GetObject("path")) &&
            !PyList_SetItem(sys_path, 0, argv0))
        {
            Py_INCREF(argv0);
            Py_DECREF(importer);
            sys_path = NULL;
            return RunModule(L"__main__", 0) != 0;
        }
    }
    Py_XDECREF(argv0);
    Py_XDECREF(importer);
    if (PyErr_Occurred()) {
        PyErr_Print();
        return 1;
    }
    else {
        return -1;
    }
}

static int
run_command(wchar_t *command, PyCompilerFlags *cf)
{
    PyObject *unicode, *bytes;
    int ret;

    unicode = PyUnicode_FromWideChar(command, -1);
    if (unicode == NULL)
        goto error;
    bytes = PyUnicode_AsUTF8String(unicode);
    Py_DECREF(unicode);
    if (bytes == NULL)
        goto error;
    ret = PyRun_SimpleStringFlags(PyBytes_AsString(bytes), cf);
    Py_DECREF(bytes);
    return ret != 0;

error:
    PySys_WriteStderr("Unable to decode the command from the command line:\n");
    PyErr_Print();
    return 1;
}

static int
run_file(FILE *fp, const wchar_t *filename, PyCompilerFlags *p_cf)
{
    PyObject *unicode, *bytes = NULL;
    char *filename_str;
    int run;

    /* call pending calls like signal handlers (SIGINT) */
    if (Py_MakePendingCalls() == -1) {
        PyErr_Print();
        return 1;
    }

    if (filename) {
        unicode = PyUnicode_FromWideChar(filename, wcslen(filename));
        if (unicode != NULL) {
            bytes = PyUnicode_AsUTF8String(unicode);
            Py_DECREF(unicode);
        }
        if (bytes != NULL)
            filename_str = PyBytes_AsString(bytes);
        else {
            PyErr_Clear();
            filename_str = "<decoding error>";
        }
    }
    else
        filename_str = "<stdin>";

    run = PyRun_AnyFileExFlags(fp, filename_str, filename != NULL, p_cf);
    Py_XDECREF(bytes);
    return run != 0;
}


/* Main program */

int
Py_Main(int argc, wchar_t **argv)
{
    int c;
    int sts;
    wchar_t *command = NULL;
    wchar_t *filename = NULL;
    wchar_t *module = NULL;
    FILE *fp = stdin;
    char *p;
#ifdef MS_WINDOWS
    wchar_t *wp;
#endif
    int skipfirstline = 0;
    int stdin_is_interactive = 0;
    int help = 0;
    int version = 0;
    int saw_unbuffered_flag = 0;
    PyCompilerFlags cf;

    cf.cf_flags = 0;

    orig_argc = argc;           /* For Py_GetArgcArgv() */
    orig_argv = argv;

    PySys_ResetWarnOptions();

    while ((c = _PyOS_GetOpt(argc, argv, PROGRAM_OPTS)) != EOF) {
        if (c == 'c') {
            size_t len;
            /* -c is the last option; following arguments
               that look like options are left for the
               command to interpret. */

            len = wcslen(_PyOS_optarg) + 1 + 1;
            command = (wchar_t *)malloc(sizeof(wchar_t) * len);
            if (command == NULL)
                Py_FatalError(
                   "not enough memory to copy -c argument");
            wcscpy(command, _PyOS_optarg);
            command[len - 2] = '\n';
            command[len - 1] = 0;
            break;
        }

        if (c == 'm') {
            /* -m is the last option; following arguments
               that look like options are left for the
               module to interpret. */
            module = _PyOS_optarg;
            break;
        }

        switch (c) {
        case 'b':
            Py_BytesWarningFlag++;
            break;

        case 'd':
            Py_DebugFlag++;
            break;

        case 'i':
            Py_InspectFlag++;
            Py_InteractiveFlag++;
            break;

        /* case 'J': reserved for Jython */

        case 'O':
            Py_OptimizeFlag++;
            break;

        case 'B':
            Py_DontWriteBytecodeFlag++;
            break;

        case 's':
            Py_NoUserSiteDirectory++;
            break;

        case 'S':
            Py_NoSiteFlag++;
            break;

        case 'E':
            Py_IgnoreEnvironmentFlag++;
            break;

        case 't':
            /* ignored for backwards compatibility */
            break;

        case 'u':
            Py_UnbufferedStdioFlag = 1;
            saw_unbuffered_flag = 1;
            break;

        case 'v':
            Py_VerboseFlag++;
            break;

        case 'x':
            skipfirstline = 1;
            break;

        /* case 'X': reserved for implementation-specific arguments */

        case 'h':
        case '?':
            help++;
            break;

        case 'V':
            version++;
            break;

        case 'W':
            PySys_AddWarnOption(_PyOS_optarg);
            break;

        /* This space reserved for other options */

        default:
            return usage(2, argv[0]);
            /*NOTREACHED*/

        }
    }

    if (help)
        return usage(0, argv[0]);

    if (version) {
        fprintf(stderr, "Python %s\n", PY_VERSION);
        return 0;
    }

    if (!Py_InspectFlag &&
        (p = Py_GETENV("PYTHONINSPECT")) && *p != '\0')
        Py_InspectFlag = 1;
    if (!saw_unbuffered_flag &&
        (p = Py_GETENV("PYTHONUNBUFFERED")) && *p != '\0')
        Py_UnbufferedStdioFlag = 1;

    if (!Py_NoUserSiteDirectory &&
        (p = Py_GETENV("PYTHONNOUSERSITE")) && *p != '\0')
        Py_NoUserSiteDirectory = 1;

#ifdef MS_WINDOWS
    if (!Py_IgnoreEnvironmentFlag && (wp = _wgetenv(L"PYTHONWARNINGS")) &&
        *wp != L'\0') {
        wchar_t *buf, *warning;

        buf = (wchar_t *)malloc((wcslen(wp) + 1) * sizeof(wchar_t));
        if (buf == NULL)
            Py_FatalError(
               "not enough memory to copy PYTHONWARNINGS");
        wcscpy(buf, wp);
        for (warning = wcstok(buf, L",");
             warning != NULL;
             warning = wcstok(NULL, L",")) {
            PySys_AddWarnOption(warning);
        }
        free(buf);
    }
#else
    if ((p = Py_GETENV("PYTHONWARNINGS")) && *p != '\0') {
        char *buf, *oldloc;
        PyObject *warning;

        /* settle for strtok here as there's no one standard
           C89 wcstok */
        buf = (char *)malloc(strlen(p) + 1);
        if (buf == NULL)
            Py_FatalError(
               "not enough memory to copy PYTHONWARNINGS");
        strcpy(buf, p);
        oldloc = strdup(setlocale(LC_ALL, NULL));
        setlocale(LC_ALL, "");
        for (p = strtok(buf, ","); p != NULL; p = strtok(NULL, ",")) {
            warning = PyUnicode_DecodeFSDefault(p);
            if (warning != NULL) {
                PySys_AddWarnOptionUnicode(warning);
                Py_DECREF(warning);
            }
        }
        setlocale(LC_ALL, oldloc);
        free(oldloc);
        free(buf);
    }
#endif

    if (command == NULL && module == NULL && _PyOS_optind < argc &&
        wcscmp(argv[_PyOS_optind], L"-") != 0)
    {
#ifdef __VMS
        filename = decc$translate_vms(argv[_PyOS_optind]);
        if (filename == (char *)0 || filename == (char *)-1)
            filename = argv[_PyOS_optind];

#else
        filename = argv[_PyOS_optind];
#endif
    }

    stdin_is_interactive = Py_FdIsInteractive(stdin, (char *)0);

    if (Py_UnbufferedStdioFlag) {
#if defined(MS_WINDOWS) || defined(__CYGWIN__)
        _setmode(fileno(stdin), O_BINARY);
        _setmode(fileno(stdout), O_BINARY);
#endif
#ifdef HAVE_SETVBUF
        setvbuf(stdin,  (char *)NULL, _IONBF, BUFSIZ);
        setvbuf(stdout, (char *)NULL, _IONBF, BUFSIZ);
        setvbuf(stderr, (char *)NULL, _IONBF, BUFSIZ);
#else /* !HAVE_SETVBUF */
        setbuf(stdin,  (char *)NULL);
        setbuf(stdout, (char *)NULL);
        setbuf(stderr, (char *)NULL);
#endif /* !HAVE_SETVBUF */
    }
    else if (Py_InteractiveFlag) {
#ifdef MS_WINDOWS
        /* Doesn't have to have line-buffered -- use unbuffered */
        /* Any set[v]buf(stdin, ...) screws up Tkinter :-( */
        setvbuf(stdout, (char *)NULL, _IONBF, BUFSIZ);
#else /* !MS_WINDOWS */
#ifdef HAVE_SETVBUF
        setvbuf(stdin,  (char *)NULL, _IOLBF, BUFSIZ);
        setvbuf(stdout, (char *)NULL, _IOLBF, BUFSIZ);
#endif /* HAVE_SETVBUF */
#endif /* !MS_WINDOWS */
        /* Leave stderr alone - it should be unbuffered anyway. */
    }
#ifdef __VMS
    else {
        setvbuf (stdout, (char *)NULL, _IOLBF, BUFSIZ);
    }
#endif /* __VMS */

#ifdef __APPLE__
    /* On MacOS X, when the Python interpreter is embedded in an
       application bundle, it gets executed by a bootstrapping script
       that does os.execve() with an argv[0] that's different from the
       actual Python executable. This is needed to keep the Finder happy,
       or rather, to work around Apple's overly strict requirements of
       the process name. However, we still need a usable sys.executable,
       so the actual executable path is passed in an environment variable.
       See Lib/plat-mac/bundlebuiler.py for details about the bootstrap
       script. */
    if ((p = Py_GETENV("PYTHONEXECUTABLE")) && *p != '\0') {
        wchar_t* buffer;
        size_t len = strlen(p);
        size_t r;

        buffer = malloc(len * sizeof(wchar_t));
        if (buffer == NULL) {
            Py_FatalError(
               "not enough memory to copy PYTHONEXECUTABLE");
        }

        r = mbstowcs(buffer, p, len);
        Py_SetProgramName(buffer);
        /* buffer is now handed off - do not free */
    } else {
        Py_SetProgramName(argv[0]);
    }
#else
    Py_SetProgramName(argv[0]);
#endif
    Py_Initialize();

    if (Py_VerboseFlag ||
        (command == NULL && filename == NULL && module == NULL && stdin_is_interactive)) {
        fprintf(stderr, "Python %s on %s\n",
            Py_GetVersion(), Py_GetPlatform());
        if (!Py_NoSiteFlag)
            fprintf(stderr, "%s\n", COPYRIGHT);
    }

    if (command != NULL) {
        /* Backup _PyOS_optind and force sys.argv[0] = '-c' */
        _PyOS_optind--;
        argv[_PyOS_optind] = L"-c";
    }

    if (module != NULL) {
        /* Backup _PyOS_optind and force sys.argv[0] = '-m'*/
        _PyOS_optind--;
        argv[_PyOS_optind] = L"-m";
    }

    PySys_SetArgv(argc-_PyOS_optind, argv+_PyOS_optind);

    if ((Py_InspectFlag || (command == NULL && filename == NULL && module == NULL)) &&
        isatty(fileno(stdin))) {
        PyObject *v;
        v = PyImport_ImportModule("readline");
        if (v == NULL)
            PyErr_Clear();
        else
            Py_DECREF(v);
    }

    if (command) {
        sts = run_command(command, &cf);
        free(command);
    } else if (module) {
        sts = RunModule(module, 1);
    }
    else {

        if (filename == NULL && stdin_is_interactive) {
            Py_InspectFlag = 0; /* do exit on SystemExit */
            RunStartupFile(&cf);
        }
        /* XXX */

        sts = -1;               /* keep track of whether we've already run __main__ */

        if (filename != NULL) {
            sts = RunMainFromImporter(filename);
        }

        if (sts==-1 && filename!=NULL) {
            if ((fp = _Py_wfopen(filename, L"r")) == NULL) {
                char cfilename[PATH_MAX];
                size_t r = wcstombs(cfilename, filename, PATH_MAX);
                if (r == PATH_MAX)
                    /* cfilename is not null-terminated;
                     * forcefully null-terminating it
                     * might break the shift state */
                    strcpy(cfilename, "<file name too long>");
                if (r == ((size_t)-1))
                    strcpy(cfilename, "<unprintable file name>");
                fprintf(stderr, "%ls: can't open file '%s': [Errno %d] %s\n",
                    argv[0], cfilename, errno, strerror(errno));

                return 2;
            }
            else if (skipfirstline) {
                int ch;
                /* Push back first newline so line numbers
                   remain the same */
                while ((ch = getc(fp)) != EOF) {
                    if (ch == '\n') {
                        (void)ungetc(ch, fp);
                        break;
                    }
                }
            }
            {
                /* XXX: does this work on Win/Win64? (see posix_fstat) */
                struct stat sb;
                if (fstat(fileno(fp), &sb) == 0 &&
                    S_ISDIR(sb.st_mode)) {
                    fprintf(stderr, "%ls: '%ls' is a directory, cannot continue\n", argv[0], filename);
                    fclose(fp);
                    return 1;
                }
            }
        }

        if (sts == -1)
            sts = run_file(fp, filename, &cf);
    }

    /* Check this environment variable at the end, to give programs the
     * opportunity to set it from Python.
     */
    if (!Py_InspectFlag &&
        (p = Py_GETENV("PYTHONINSPECT")) && *p != '\0')
    {
        Py_InspectFlag = 1;
    }

    if (Py_InspectFlag && stdin_is_interactive &&
        (filename != NULL || command != NULL || module != NULL)) {
        Py_InspectFlag = 0;
        /* XXX */
        sts = PyRun_AnyFileFlags(stdin, "<stdin>", &cf) != 0;
    }

    Py_Finalize();

#ifdef __INSURE__
    /* Insure++ is a memory analysis tool that aids in discovering
     * memory leaks and other memory problems.  On Python exit, the
     * interned string dictionaries are flagged as being in use at exit
     * (which it is).  Under normal circumstances, this is fine because
     * the memory will be automatically reclaimed by the system.  Under
     * memory debugging, it's a huge source of useless noise, so we
     * trade off slower shutdown for less distraction in the memory
     * reports.  -baw
     */
    _Py_ReleaseInternedStrings();
    _Py_ReleaseInternedUnicodeStrings();
#endif /* __INSURE__ */

    return sts;
}

/* this is gonna seem *real weird*, but if you put some other code between
   Py_Main() and Py_GetArgcArgv() you will need to adjust the test in the
   while statement in Misc/gdbinit:ppystack */

/* Make the *original* argc/argv available to other modules.
   This is rare, but it is needed by the secureware extension. */

void
Py_GetArgcArgv(int *argc, wchar_t ***argv)
{
    *argc = orig_argc;
    *argv = orig_argv;
}


/* Encode a (wide) character string to the locale encoding with the
   surrogateescape error handler (characters in range U+DC80..U+DCFF are
   converted to bytes 0x80..0xFF).

   This function is the reverse of _Py_char2wchar().

   Return a pointer to a newly allocated byte string (use PyMem_Free() to free
   the memory), or NULL on error (conversion error or memory error). */
char*
_Py_wchar2char(const wchar_t *text)
{
    const size_t len = wcslen(text);
    char *result = NULL, *bytes = NULL;
    size_t i, size, converted;
    wchar_t c, buf[2];

    /* The function works in two steps:
       1. compute the length of the output buffer in bytes (size)
       2. outputs the bytes */
    size = 0;
    buf[1] = 0;
    while (1) {
        for (i=0; i < len; i++) {
            c = text[i];
            if (c >= 0xdc80 && c <= 0xdcff) {
                /* UTF-8b surrogate */
                if (bytes != NULL) {
                    *bytes++ = c - 0xdc00;
                    size--;
                }
                else
                    size++;
                continue;
            }
            else {
                buf[0] = c;
                if (bytes != NULL)
                    converted = wcstombs(bytes, buf, size);
                else
                    converted = wcstombs(NULL, buf, 0);
                if (converted == (size_t)-1) {
                    if (result != NULL)
                        PyMem_Free(result);
                    return NULL;
                }
                if (bytes != NULL) {
                    bytes += converted;
                    size -= converted;
                }
                else
                    size += converted;
            }
        }
        if (result != NULL) {
            *bytes = 0;
            break;
        }

        size += 1; /* nul byte at the end */
        result = PyMem_Malloc(size);
        if (result == NULL)
            return NULL;
        bytes = result;
    }
    return result;
}


/* Decode a byte string from the locale encoding with the
   surrogateescape error handler (undecodable bytes are decoded as characters
   in range U+DC80..U+DCFF). If a byte sequence can be decoded as a surrogate
   character, escape the bytes using the surrogateescape error handler instead
   of decoding them.

   Use _Py_wchar2char() to encode the character string back to a byte string.

   Return a pointer to a newly allocated (wide) character string (use
   PyMem_Free() to free the memory), or NULL on error (conversion error or
   memory error). */
wchar_t*
_Py_char2wchar(char* arg)
{
    wchar_t *res;
#ifdef HAVE_BROKEN_MBSTOWCS
    /* Some platforms have a broken implementation of
     * mbstowcs which does not count the characters that
     * would result from conversion.  Use an upper bound.
     */
    size_t argsize = strlen(arg);
#else
    size_t argsize = mbstowcs(NULL, arg, 0);
#endif
    size_t count;
    unsigned char *in;
    wchar_t *out;
#ifdef HAVE_MBRTOWC
    mbstate_t mbs;
#endif
    if (argsize != (size_t)-1) {
        res = (wchar_t *)PyMem_Malloc((argsize+1)*sizeof(wchar_t));
        if (!res)
            goto oom;
        count = mbstowcs(res, arg, argsize+1);
        if (count != (size_t)-1) {
            wchar_t *tmp;
            /* Only use the result if it contains no
               surrogate characters. */
            for (tmp = res; *tmp != 0 &&
                         (*tmp < 0xd800 || *tmp > 0xdfff); tmp++)
                ;
            if (*tmp == 0)
                return res;
        }
        PyMem_Free(res);
    }
    /* Conversion failed. Fall back to escaping with surrogateescape. */
#ifdef HAVE_MBRTOWC
    /* Try conversion with mbrtwoc (C99), and escape non-decodable bytes. */

    /* Overallocate; as multi-byte characters are in the argument, the
       actual output could use less memory. */
    argsize = strlen(arg) + 1;
    res = (wchar_t*)PyMem_Malloc(argsize*sizeof(wchar_t));
    if (!res) goto oom;
    in = (unsigned char*)arg;
    out = res;
    memset(&mbs, 0, sizeof mbs);
    while (argsize) {
        size_t converted = mbrtowc(out, (char*)in, argsize, &mbs);
        if (converted == 0)
            /* Reached end of string; null char stored. */
            break;
        if (converted == (size_t)-2) {
            /* Incomplete character. This should never happen,
               since we provide everything that we have -
               unless there is a bug in the C library, or I
               misunderstood how mbrtowc works. */
            fprintf(stderr, "unexpected mbrtowc result -2\n");
            return NULL;
        }
        if (converted == (size_t)-1) {
            /* Conversion error. Escape as UTF-8b, and start over
               in the initial shift state. */
            *out++ = 0xdc00 + *in++;
            argsize--;
            memset(&mbs, 0, sizeof mbs);
            continue;
        }
        if (*out >= 0xd800 && *out <= 0xdfff) {
            /* Surrogate character.  Escape the original
               byte sequence with surrogateescape. */
            argsize -= converted;
            while (converted--)
                *out++ = 0xdc00 + *in++;
            continue;
        }
        /* successfully converted some bytes */
        in += converted;
        argsize -= converted;
        out++;
    }
#else
    /* Cannot use C locale for escaping; manually escape as if charset
       is ASCII (i.e. escape all bytes > 128. This will still roundtrip
       correctly in the locale's charset, which must be an ASCII superset. */
    res = PyMem_Malloc((strlen(arg)+1)*sizeof(wchar_t));
    if (!res) goto oom;
    in = (unsigned char*)arg;
    out = res;
    while(*in)
        if(*in < 128)
            *out++ = *in++;
        else
            *out++ = 0xdc00 + *in++;
    *out = 0;
#endif
    return res;
oom:
    fprintf(stderr, "out of memory\n");
    return NULL;
}

#ifdef __cplusplus
}
#endif
