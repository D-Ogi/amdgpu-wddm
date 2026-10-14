// Stored settings are untrusted as a group. Absence of FanMode requests the
// standard curve; a failed read or an invalid custom curve closes ownership.
// GuardQuerySetting and the setting-name table are supplied by fan.c.
static NTSTATUS FanReadChoice(struct bc250_fan_request* Start, ULONG* Points)
{
    ULONG mode = 0, profile = BC250_FAN_PROFILE_STANDARD, points = 0, value = 0, i;
    struct bc250_fan_curve resolved;
    NTSTATUS status;

    RtlZeroMemory(Start, sizeof(*Start));
    *Points = 0;
    status = GuardQuerySetting(FAN_SETTING_MODE, &mode);
    if (!NT_SUCCESS(status)) return status;
    status = GuardQuerySetting(FAN_SETTING_PROFILE, &profile);
    if (status != STATUS_OBJECT_NAME_NOT_FOUND && !NT_SUCCESS(status)) return status;
    Start->mode = mode;
    Start->profile = profile;
    if (mode == BC250_FAN_MODE_CURVE && profile == BC250_FAN_PROFILE_CUSTOM) {
        status = GuardQuerySetting(FAN_SETTING_POINTS, &points);
        if (!NT_SUCCESS(status)) return STATUS_DATA_ERROR;
        if (points > BC250_FAN_POINTS_MAX) return STATUS_DATA_ERROR;
        Start->curve.points = points;
        for (i = 0; i < points; i++) {
            status = GuardQuerySetting(g_FanCurveSetting[i], &value);
            if (!NT_SUCCESS(status) || (value & 0xFFFF0000u)) return STATUS_DATA_ERROR;
            Start->curve.p[i].c = (value >> 8) & 0xFFu;
            Start->curve.p[i].pct = value & 0xFFu;
        }
        *Points = points;
    }
    if (mode == BC250_FAN_MODE_FIXED ||
        bc250_fan_request_check(Start, &resolved) != BC250_FAN_ERROR_OK)
        return STATUS_DATA_ERROR;
    return STATUS_SUCCESS;
}

// Resolve the final setting before ownership is enabled. A late query failure
// can close the group after the earlier device-start validation succeeded.
static NTSTATUS FanReadConfiguredChoice(struct bc250_fan_request* Start, ULONG* Points, ULONG* Boost)
{
    NTSTATUS choice = FanReadChoice(Start, Points), status;
    *Boost = 0;
    if (!NT_SUCCESS(choice) && choice != STATUS_OBJECT_NAME_NOT_FOUND) return choice;
    status = GuardQuerySetting(FAN_SETTING_BOOST, Boost);
    if (!NT_SUCCESS(status)) return status == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_DATA_ERROR : status;
    if (!DriverStateGroupEnabled(AMDGPU_STATE_GROUP_FAN)) return STATUS_DATA_ERROR;
    return choice;
}
