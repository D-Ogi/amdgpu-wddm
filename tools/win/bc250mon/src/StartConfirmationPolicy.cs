// Pure observed-progress policy. No registry, transport, UI or wall-clock dependence.
using System;
namespace Bc250Mon
{
    public sealed class StartConfirmationPolicy
    {
        public const long RequiredMilliseconds = 60000;
        public const long MaximumGapMilliseconds = 15000;
        bool _observing;
        long _since, _lastSample;
        ulong _generation, _epoch, _completed, _readyAge;
        public long ObservedMilliseconds { get; private set; }
        public ulong Generation { get { return _generation; } }
        public ulong Epoch { get { return _epoch; } }

        public void Reset()
        {
            _observing = false;
            ObservedMilliseconds = 0;
            _generation = _epoch = _completed = _readyAge = 0;
        }
        void Begin(long now, StartHealthSnapshot sample)
        {
            _observing = true; _since = _lastSample = now;
            _generation = sample.Generation; _epoch = sample.Epoch;
            _completed = sample.Completed; _readyAge = sample.ReadyAgeMs;
            ObservedMilliseconds = 0;
        }
        public bool Observe(long now, StartHealthSnapshot sample)
        {
            if ((sample.Flags & 7) != 7 || (sample.Flags & 8) != 0 || sample.Generation == 0 || sample.Epoch == 0 ||
                sample.Completed == 0 || sample.LastCompletionAgeMs > (ulong)MaximumGapMilliseconds)
            { Reset(); return false; }
            if (!_observing || sample.Generation != _generation || sample.Epoch != _epoch ||
                now < _lastSample || now - _lastSample > MaximumGapMilliseconds)
            { Begin(now, sample); return false; }
            // Repeated reads of one completed surface are not new presentation.
            if (sample.Completed <= _completed || sample.ReadyAgeMs < _readyAge)
            { Reset(); return false; }
            _lastSample = now; _completed = sample.Completed; _readyAge = sample.ReadyAgeMs;
            ObservedMilliseconds = now - _since;
            return ObservedMilliseconds >= RequiredMilliseconds && sample.ReadyAgeMs >= (ulong)RequiredMilliseconds;
        }
    }
}
