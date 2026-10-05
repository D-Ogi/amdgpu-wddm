# M743 - Cross-session nested launch refusal isolated without D3D

Console002 source98558e50, manifest
8EDA6F131676F2EC604BD2873FBA6D8BDCF87F48B2F8232A9E92B450843DC8DE.
The no-GPU control reproduces launch_error5 specifically at CreateProcessAsUser,
after LocalSystem/token/session/environment admission. Outer Job root exits1
(helper126), job_empty=true. Baseline postflight passes, same CPU171 boot and
driver generation. Overall14.3462954s; task removed and Missing observed.
No driver routing or D3D workload. Raw scratch/m14/console002-ops; launch-error
is a reduced structured excerpt from test.err, not its entire contents.

Microsoft documents same-session membership for a Job Object:
https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject
Together with standalone M740 success, the measurement supports separating
session0 controller and user-session Jobs. It does not justify bypassing ownership
or accepting a missing closure receipt. A monitor/cancellation path is prepared:
SYSTEM supervisor remains outside the client Job; explicit cancel requests cause
helper termination and a job-empty receipt before any restoration.

Host tests pass for pre-resume cancellation, cancellation of a running descendant
tree, injected monitor failure with confirmed closure, and the existing success,
quoting, error, timeout/orphan/tree regressions. No new native Present result.
