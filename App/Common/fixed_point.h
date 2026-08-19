#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <stdint.h>

/** Q16.16 фиксированная точка. Один int32_t — атомарный read/write на Cortex-M4. */
typedef int32_t fixed_t;

#define FIXED_SHIFT        16
#define FIXED_ONE          (1 << FIXED_SHIFT)

#define FIXED_FROM_INT(i)    ((fixed_t)((int32_t)(i) << FIXED_SHIFT))
#define FIXED_TO_INT(f)      ((int32_t)((f) >> FIXED_SHIFT))
#define FIXED_FROM_FLOAT(x)  ((fixed_t)((x) * FIXED_ONE))   /* только для констант на этапе компиляции */
#define FIXED_TO_FLOAT(f)    ((float)(f) / (float)FIXED_ONE) /* только для вывода на экран/отладки */

static inline fixed_t fixed_mul(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

static inline fixed_t fixed_div(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

#endif /* FIXED_POINT_H */