# M393 - Persist RLC and deferred visibility boundaries

Add an optional callback around the existing stage5 body and deferred GFX commit. Restrict persistence to unpublished full-WDDM RUN ending atstage5 atPASSIVE with special APCs enabled. Extend the CP1 unsafe-fast-mutex branch and matched release for this caller. No MMIO sequence change. Exercise actual RunEngineStage/commit with and without callback; negative omission must fail. Build WDK and package122; no hardware claim until trial.
