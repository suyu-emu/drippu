# Experimental integrations

## NSO / Prelude stub

`src/nintendo_library` is an interface stub only. It deliberately does not send Nintendo
credentials, scrape Nintendo endpoints, or claim official-service compatibility. `StartAuthentication`
always returns `false` with `ServiceUnavailable` and records a diagnostic explaining that the Prelude
adapter is not connected.

The `externals/Prelude-Nro` submodule is a reviewed integration seam for future work. Any adapter
must be explicitly opt-in, accept only user-supplied hardware-derived material, keep secrets out of
logs and crash reports, and provide a test/mock transport before a real transport is considered.

## NX-Optimiser

`externals/nx-optimizer` remains an optional submodule. It is not invoked automatically and no game
files are modified by drippu. A future launcher setting should pass a temporary copy/path to an
NX-Optimiser command, check its exit status, and launch the resulting artifact only when the user
has enabled the experimental option. Until that launcher wiring lands, the integration is inert.
