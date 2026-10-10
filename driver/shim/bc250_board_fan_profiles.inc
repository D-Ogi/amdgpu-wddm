// BC-250 cooling profiles. Included once by the fan policy.
static const struct bc250_fan_curve g_fan_profiles[BC250_FAN_PROFILE_COUNT] = {
	{ 0u, { { 0u, 0u } } },							/* CUSTOM: from the request */
	{ 5u, { { 40u, 50u }, { 60u, 70u }, { 70u, 85u }, { 76u, 95u }, { 80u, 100u } } },
	{ 5u, { { 40u, 30u }, { 60u, 45u }, { 70u, 60u }, { 80u, 80u }, { 85u, 100u } } },
	{ 4u, { { 40u, 60u }, { 55u, 75u }, { 65u, 90u }, { 75u, 100u } } },
};
