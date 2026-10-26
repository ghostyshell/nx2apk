// Darwin's xattr calls (position and options arguments) on Linux's (l* forms for XATTR_NOFOLLOW, which is the only option the host passes)
#pragma once
#include_next <sys/xattr.h>
#include <errno.h>
#define XATTR_NOFOLLOW 0x0001
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define getxattr(p, n, v, s, pos, opt) lgetxattr((p), (n), (v), (s))
#define setxattr(p, n, v, s, pos, opt) lsetxattr((p), (n), (v), (s), 0)
#define removexattr(p, n, opt) lremovexattr((p), (n))
