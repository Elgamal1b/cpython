@rem Used by the buildbot "compile" step.

@rem Clean up
set PLAT=
if '%1' EQU '-p' if '%2' EQU 'x64' (set PLAT=-amd64)

call "%~dp0clean%PLAT%.bat"

@rem If you need the buildbots to start fresh (such as when upgrading to
@rem a new version of an external library, especially Tcl/Tk):
@rem 1) uncomment the following line:

@rem    call "%~dp0..\..\PCbuild\get_externals.bat" --clean-only

@rem 2) commit and push
@rem 3) wait for all Windows bots to start a build with that changeset
@rem 4) re-comment, commit and push again

@rem Do the build
call "%~dp0..\..\PCbuild\build.bat" -e -d -v %*
