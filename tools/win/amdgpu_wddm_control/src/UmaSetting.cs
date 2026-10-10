using System;
using System.Globalization;
using System.Security.Cryptography;

namespace AmdgpuWddmControl
{
    public sealed class UmaState
    {
        public ulong ActiveBytes;
        public uint RequestedMiB, PreviousMiB;
        public bool ActiveValid, ReadValid, WriteAllowed, BackupAvailable;
        public uint Reason, Operation;
        public int ResultCode;
        public byte[] ObservedBlock = new byte[28];
        public string Refusal;
    }

    public static class UmaSetting
    {
        public static byte[] Request(uint op, uint target, UmaState expected)
        {
            if (op > 2 || (op == 0 && target != 0) || (op == 1 && !CanSet(expected, target)) ||
                (op == 2 && (target != 0 || !CanRestore(expected)))) throw new ArgumentException("UMA request refused");
            var b = new byte[48];
            Array.Copy(BitConverter.GetBytes(48u), 0, b, 0, 4);
            Array.Copy(BitConverter.GetBytes(op), 0, b, 4, 4);
            Array.Copy(BitConverter.GetBytes(target), 0, b, 8, 4);
            if (op != 0)
            {
                if (expected.ObservedBlock == null || expected.ObservedBlock.Length != 28) throw new ArgumentException("UMA observation missing");
                Array.Copy(expected.ObservedBlock, 0, b, 16, 28);
            }
            return b;
        }
        public static UmaState Parse(byte[] b, uint command)
        {
            if (b == null || b.Length != 128 || BitConverter.ToUInt32(b, 0) != KmdReply.Magic ||
                BitConverter.ToUInt32(b, 4) != command || BitConverter.ToUInt32(b, 16) != 1 ||
                BitConverter.ToUInt32(b, 8) != 0 || BitConverter.ToUInt32(b, 12) != 0)
                throw new FormatException("UMA reply ABI mismatch");
            uint flags = BitConverter.ToUInt32(b, 24);
            if ((flags & ~15u) != 0 || BitConverter.ToUInt32(b, 20) > 2 || BitConverter.ToUInt32(b, 28) > 6)
                throw new FormatException("UMA reply fields unsupported");
            for (int i = 80; i < 128; i++) if (b[i] != 0) throw new FormatException("UMA reserved fields nonzero");
            if ((flags & 4) != 0 && ((flags & 2) == 0 || BitConverter.ToUInt32(b, 28) != 0))
                throw new FormatException("UMA write capability inconsistent");
            var s = new UmaState { ActiveValid = (flags & 1) != 0, ReadValid = (flags & 2) != 0,
                WriteAllowed = (flags & 4) != 0, BackupAvailable = (flags & 8) != 0,
                ActiveBytes = BitConverter.ToUInt64(b, 32), RequestedMiB = BitConverter.ToUInt32(b, 40),
                PreviousMiB = BitConverter.ToUInt32(b, 44), Reason = BitConverter.ToUInt32(b, 28),
                Operation = BitConverter.ToUInt32(b, 20), ResultCode = BitConverter.ToInt32(b, 76) };
            Array.Copy(b, 48, s.ObservedBlock, 0, 28);
            return s;
        }
        public static string ConfirmationToken(UmaState state)
        {
            if (state == null || state.ObservedBlock == null || state.ObservedBlock.Length != 28) throw new ArgumentException("UMA observation missing");
            var b = new byte[48];
            Array.Copy(state.ObservedBlock, b, 28);
            Array.Copy(BitConverter.GetBytes(state.RequestedMiB), 0, b, 28, 4);
            Array.Copy(BitConverter.GetBytes(state.PreviousMiB), 0, b, 32, 4);
            Array.Copy(BitConverter.GetBytes(state.ActiveBytes), 0, b, 36, 8);
            uint flags=(state.ActiveValid?1u:0u)|(state.ReadValid?2u:0u)|(state.WriteAllowed?4u:0u)|(state.BackupAvailable?8u:0u);
            Array.Copy(BitConverter.GetBytes(flags),0,b,44,4);
            using(var sha=SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(b)).Replace("-", "");
        }
        public static bool Confirmed(UmaState state, string token)
        {
            return token != null && token.Length == 64 && string.Equals(ConfirmationToken(state), token, StringComparison.Ordinal);
        }
        public static bool Verified(UmaState result, uint operation, uint target)
        {
            return result != null && result.ReadValid && result.Reason == 0 && result.Operation == operation &&
                (result.ResultCode == 0 || result.ResultCode == 1) && result.RequestedMiB == target;
        }
        public static readonly uint[] Choices = { 8192, 12288 };
        public static bool Offered(uint mib) { return mib == 8192 || mib == 12288; }
        public static bool CanSet(UmaState state, uint mib)
        {
            return state != null && state.ReadValid && state.WriteAllowed && state.Reason == 0 && Offered(mib) && state.RequestedMiB != mib;
        }
        public static bool CanRestore(UmaState state)
        {
            return state != null && state.ReadValid && state.WriteAllowed && state.Reason == 0 && state.BackupAvailable && Offered(state.PreviousMiB);
        }
        public static bool Pending(UmaState state)
        {
            return state != null && state.ActiveValid && state.ReadValid && state.ActiveBytes != (ulong)state.RequestedMiB * 1024 * 1024;
        }
        public static string Size(uint mib) { return (mib / 1024.0).ToString("0.##", CultureInfo.CurrentCulture) + " GiB"; }
        public static string Active(UmaState state)
        {
            return state == null || !state.ActiveValid ? Strings.T("uma.unavailable") :
                (state.ActiveBytes / 1073741824.0).ToString("0.##", CultureInfo.CurrentCulture) + " GiB";
        }
    }
}
