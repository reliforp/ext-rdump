PHP_ARG_ENABLE([rdump],
  [whether to enable rdump support],
  [AS_HELP_STRING([--enable-rdump],
    [Enable rdump (self-process memory dump in reli RDUMP format)])],
  [no])

if test "$PHP_RDUMP" != "no"; then
  AC_DEFINE(HAVE_RDUMP, 1, [ Have rdump support ])
  PHP_NEW_EXTENSION(rdump, rdump.c, $ext_shared)
fi
