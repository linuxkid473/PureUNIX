
#ifndef LIBFM_QT_API_H
#define LIBFM_QT_API_H

#ifdef FM_QT6_STATIC_DEFINE
#  define LIBFM_QT_API
#  define FM_QT6_NO_EXPORT
#else
#  ifndef LIBFM_QT_API
#    ifdef fm_qt6_EXPORTS
        /* We are building this library */
#      define LIBFM_QT_API 
#    else
        /* We are using this library */
#      define LIBFM_QT_API 
#    endif
#  endif

#  ifndef FM_QT6_NO_EXPORT
#    define FM_QT6_NO_EXPORT 
#  endif
#endif

#ifndef FM_QT6_DEPRECATED
#  define FM_QT6_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef FM_QT6_DEPRECATED_EXPORT
#  define FM_QT6_DEPRECATED_EXPORT LIBFM_QT_API FM_QT6_DEPRECATED
#endif

#ifndef FM_QT6_DEPRECATED_NO_EXPORT
#  define FM_QT6_DEPRECATED_NO_EXPORT FM_QT6_NO_EXPORT FM_QT6_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef FM_QT6_NO_DEPRECATED
#    define FM_QT6_NO_DEPRECATED
#  endif
#endif

#endif /* LIBFM_QT_API_H */
