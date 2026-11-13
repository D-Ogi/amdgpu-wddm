/* SPDX-License-Identifier: BSD-3-Clause */
/* The power and command signals of the libtpms TPM 2 platform layer. Upstream they are in
   src/tpm2/TPMCmd/Simulator/src/TPMCmdp.c next to a TCP server; on Windows that file needs the simulator socket
   headers. These are the same bodies without the socket: power on and reset reach _plat__Signal_PowerOn and
   _plat__Signal_Reset, where _TPM_Init runs. Without them every command fails with FATAL_ERROR_NO_INIT (503). */
#include <stdbool.h>
#include "Platform.h"
#include "TpmTcpProtocol.h"

static bool s_isPowerOn = false;

void _rpc__Send_Command(unsigned char locality, _IN_BUFFER request, _OUT_BUFFER *response)
{
	if (!s_isPowerOn) {
		response->BufferSize = 0;
		return;
	}
	_plat__LocalitySet(locality);
	_plat__RunCommand(request.BufferSize, request.Buffer, &response->BufferSize, &response->Buffer);
}

void _rpc__Signal_PowerOn(bool isReset)
{
	if (s_isPowerOn && !isReset) return;
	if (isReset && !s_isPowerOn) return;
	if (!isReset) _plat__Signal_PowerOn();
	_plat__Signal_Reset();
	s_isPowerOn = true;
}

void _rpc__Signal_PowerOff(void)
{
	if (!s_isPowerOn) return;
	_plat__Signal_PowerOff();
	s_isPowerOn = false;
}

int _rpc__Signal_GetTPMEstablished(void) { return 0; }
int _rpc__Signal_SetTPMEstablished(int value) { (void)value; return 0; }
int _rpc__Signal_ResetTPMEstablished(void) { return 0; }
int _rpc__Signal_NvOn(void) { return 0; }
bool _rpc__Signal_IsPowerOn(void) { return s_isPowerOn; }
int _rpc__Signal_CancelOn(void) { return 0; }
int _rpc__Signal_CancelOff(void) { return 0; }
