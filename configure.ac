AC_PREREQ([2.69])
AC_INIT([walkies], [0.1.0], [https://github.com/nzmacgeek/walkies/issues])
AC_CONFIG_SRCDIR([user/walkies.c])

AC_PROG_CC

AC_ARG_VAR([BUILD_DIR], [output directory for build artefacts])
AC_ARG_VAR([BLUEYOS_SYSROOT], [default BlueyOS sysroot path])
AC_ARG_VAR([SYSROOT], [target BlueyOS rootfs for dimsim offline installs])
AC_ARG_VAR([MUSL_PREFIX], [path to an installed musl-blueyos sysroot])
AC_ARG_VAR([DPKBUILD], [path to the dpkbuild executable])
AC_ARG_VAR([DIMSIM], [path to the dimsim executable])

AC_ARG_ENABLE(
  [debug],
  [AS_HELP_STRING([--enable-debug], [build with debug flags (-g -O0 -DDEBUG)])],
  [DEBUG=1],
  [DEBUG=0]
)

AS_IF([test -z "$BUILD_DIR"], [BUILD_DIR=build])
AS_IF([test -z "$BLUEYOS_SYSROOT"], [BLUEYOS_SYSROOT=/opt/blueyos-sysroot])
AS_IF([test -z "$SYSROOT"], [SYSROOT=$BLUEYOS_SYSROOT])

AS_IF([test -z "$MUSL_PREFIX"], [
  for candidate in "$SYSROOT" "$BLUEYOS_SYSROOT"; do
    AS_IF(
      [test -n "$candidate" && test -d "$candidate/include" && test -f "$candidate/lib/libc.a"],
      [MUSL_PREFIX=$candidate; break]
    )
  done
])
AS_IF([test -z "$MUSL_PREFIX"], [MUSL_PREFIX=$BUILD_DIR/musl])

AC_PATH_PROG([DPKBUILD_PATH], [dpkbuild], [])
AC_PATH_PROG([DIMSIM_PATH], [dimsim], [])

AS_IF([test -z "$DPKBUILD"], [
  AS_IF([test -n "$DPKBUILD_PATH"], [DPKBUILD=$DPKBUILD_PATH], [DPKBUILD=dpkbuild])
])
AS_IF([test -z "$DIMSIM"], [
  AS_IF([test -n "$DIMSIM_PATH"], [DIMSIM=$DIMSIM_PATH], [DIMSIM=dimsim])
])

AS_IF(
  [test -z "$DPKBUILD_PATH"],
  [AC_MSG_WARN([dpkbuild was not found in PATH; make package will require DPKBUILD to resolve later])]
)
AS_IF(
  [test -z "$DIMSIM_PATH"],
  [AC_MSG_WARN([dimsim was not found in PATH; make install will require DIMSIM to resolve later])]
)

AC_SUBST([BUILD_DIR])
AC_SUBST([BLUEYOS_SYSROOT])
AC_SUBST([SYSROOT])
AC_SUBST([MUSL_PREFIX])
AC_SUBST([DPKBUILD])
AC_SUBST([DIMSIM])
AC_SUBST([DEBUG])
AC_SUBST([CC])

AC_CONFIG_FILES([config.mk])
AC_OUTPUT

AC_MSG_NOTICE([])
AC_MSG_NOTICE([Configuration summary:])
AC_MSG_NOTICE([  CC              = $CC])
AC_MSG_NOTICE([  BUILD_DIR       = $BUILD_DIR])
AC_MSG_NOTICE([  BLUEYOS_SYSROOT = $BLUEYOS_SYSROOT])
AC_MSG_NOTICE([  SYSROOT         = $SYSROOT])
AC_MSG_NOTICE([  MUSL_PREFIX     = $MUSL_PREFIX])
AC_MSG_NOTICE([  DPKBUILD        = $DPKBUILD])
AC_MSG_NOTICE([  DIMSIM          = $DIMSIM])
AC_MSG_NOTICE([  DEBUG           = $DEBUG])
AC_MSG_NOTICE([])
AC_MSG_NOTICE([Run 'make' to build, 'make package' to create the .dpk, or 'make install' to install it into SYSROOT.])