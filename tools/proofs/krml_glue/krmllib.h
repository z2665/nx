/* 前置 krmllib.h：先补 compat.h（krml_checked_int_t），再 include_next 真伞 */
#ifndef KRML_GLUE_KRMLLIB_H
#define KRML_GLUE_KRMLLIB_H
#include "krml/internal/compat.h"
#include_next "krmllib.h"
#endif
