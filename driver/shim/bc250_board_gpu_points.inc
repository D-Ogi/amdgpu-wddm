// BC-250 qualified operating points. Included once by the clock policy.
// The last three rows are the overclock points (bc250_oc.h): only a tier that admits a point makes it reachable.
const struct bc250_clock_point bc250_clock_points[BC250_CLOCK_ALL_LEVELS]={
    {500,820,116},{600,820,116},{700,820,116},{800,820,116},{900,820,116},
    {1000,820,116},{1100,840,113},{1200,860,110},{1300,880,107},{1400,899,104},{1500,919,100},
    {1600,935,98},{1700,952,95},{1800,968,93},{1900,984,90},{2000,1000,88},
    {2100,1025,84},{2200,1050,80},{2300,1075,76}
};
