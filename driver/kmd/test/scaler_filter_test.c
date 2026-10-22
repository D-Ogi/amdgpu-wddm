#include "../../shim/bc250_scaler_filter.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    BC250_SCALER_FILTER_PLAN plan;
    unsigned kind, taps, ratio, i;
    int read;
    if (argc != 2 || strcmp(argv[1], "--stream")) return 2;
    if (Bc250ScalerFilterPlan(0, 4, 524288, NULL) != 0) return 3;
    while ((read = scanf_s("%u %u %u", &kind, &taps, &ratio)) == 3) {
        int ok;
        memset(&plan, 0xa5, sizeof(plan));
        ok = Bc250ScalerFilterPlan(kind, taps, ratio, &plan);
        printf("%d %u %u %u", ok, plan.Taps, plan.PairsPerPhase, plan.CoefficientCount);
        for (i = 0; i < 33 * 8; ++i) printf(" %u", plan.Coefficients[i]);
        putchar('\n');
    }
    return read == EOF ? 0 : 2;
}
