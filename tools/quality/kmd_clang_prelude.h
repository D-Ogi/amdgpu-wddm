/* Forced first include (/FI) of the clang replay of the KMD build (tools/quality/kmd_clang.py). Never in a build.
 *
 * wdm.h of WDK 10.0.26100 declares KBUGCHECK_REASON_CALLBACK_ROUTINE with a parameter of type
 * struct _KBUGCHECK_REASON_CALLBACK_RECORD * before it declares that struct. In standard C the tag then has
 * prototype scope, so the role type matches no function: clang refuses driver/kmd/hang.c with "conflicting types
 * for 'HangAddPages'" and "incompatible function pointer types". cl.exe accepts it. This declaration of the tag at
 * file scope, before wdm.h, gives the prototype the same struct as the definition, which is what cl.exe does. */
struct _KBUGCHECK_REASON_CALLBACK_RECORD;
