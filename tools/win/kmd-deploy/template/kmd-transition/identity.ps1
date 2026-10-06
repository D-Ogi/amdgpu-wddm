# HOST-TEST FIXTURE (174 over 173). stage.py freeze replaces this file in every attempt with one generated from
# the candidate and rollback package manifests and native-caps001/lab-baseline.json. The generated file is the
# only place that names a promotion. Candidate hashes are not here: stage.py freezes them into package-hashes.json from the built
# package, and the stage manifest binds that file. The KMD171 postflight once failed only because one script kept
# an ABI170 literal; test-identity.ps1 rejects version, ABI and path literals anywhere else in this tree.
$KmdCandidateLabel='candidate174'
$KmdCandidateVersion='0.7.174.1'
$KmdCandidateAbi='0x000700AE'
$KmdRollbackLabel='rollback173'
$KmdRollbackVersion='0.7.173.1'
$KmdRollbackAbi='0x000700AD'
# The complete signed 173 package as promoted: scratch/m15/kmd173-deploy/attempts/kmd173-deploy001/candidate173
# (stage manifest 29E8F7BC, commit 13017eab, flavor package).
$KmdRollbackSysSha256='91DDEFDFC2130438BAEBF9908EDD742D79B26E2ED4973E9834C438AB8C2025B4'
$KmdRollbackInfSha256='0E8C218E67771DED68DA2F191CACC2B2DEE82D0205ACA6F0C80A2E8485C3E134'
$KmdRollbackCatSha256='06798163F4EC490365A5A1618183DF34944ECA25F5425DF009F1ADE0CB01DD81'
$KmdCandidateMode='rehearsal'
$KmdSameMode='same'
$KmdDeployMode='deploy'
$KmdDirectoryPattern='^C:\\BC250\\m15\\kmd174-deploy[0-9]{3}$'
$KmdTaskName='BC250-KMD-Watch'
# The deployed CPU desktop UMD and ICD (desktop-umd173-007, STATE.md; native-caps001/lab-baseline.json). The KMD
# promotion never writes them; preflight, Verify and postflight require both files and DWM on this UMD.
$KmdDesktopUmdPath='C:\BC250\m15\desktop-umd173-007\bc250d3d.dll'
$KmdDesktopUmdSha256='4176D1DF5E93DB284A3FADF8E406AF98E4346966E15682015D7B5BA0ED7C8544'
$KmdIcdPath='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$KmdIcdSha256='CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'
# The desktop the promotion runs under (stage.py desktop_pins): the fixture is the CPU desktop before the router,
# both switches 0 and DWM holding the CPU UMD alone. With a router: '1', router + CPU UMD, the router's key.
$KmdDesktopSwitches='0'
$KmdDesktopModules='4176D1DF5E93DB284A3FADF8E406AF98E4346966E15682015D7B5BA0ED7C8544'
$KmdDesktopRouterKey=''
