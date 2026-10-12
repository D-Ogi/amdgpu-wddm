#ifndef BC250_DML_OS_TYPES_H
#define BC250_DML_OS_TYPES_H
static inline float bc250_dml_invalid_float(void)
{
    union { unsigned int Bits; float Value; } value;
    value.Bits = 0x7fc00000u;
    return value.Value;
}
#endif
