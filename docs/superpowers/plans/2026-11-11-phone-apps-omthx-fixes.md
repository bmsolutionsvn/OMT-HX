# Phone apps: OMT-HX fixes (app review H1 phone half, P1–P3)

One plan, two repos, one run each. The brief says which repo a run is for.

| | OMT Camera | Bread OMT Screen |
|---|---|---|
| Repo | `F:\Coding\BreadMedia\opencamera-omthx` | `F:\Coding\BreadMedia\openscreenshare-omthx` |
| Base branch | `master` | `main` |
| Encoder | `app/src/main/java/net/sourceforge/opencamera/OMTHXEncoder.java` | `app/src/main/java/io/github/bachtam2001/breadomtscreen/omt/OMTHXEncoder.java` |
| Where coded frames are sent | `OMTStreamingManager.java` | `CaptureService.java` |
| Unit tests | `app/src/test/java/net/sourceforge/opencamera/test/` | `app/src/test/java/io/github/bachtam2001/breadomtscreen/omt/` |
| Tasks | 1, 2, 3, 4 | 2, 3, 4 |

Review: `F:\Coding\BreadMedia\OMT-HX\docs\reviews\2026-11-11-app-review.md`.
Find every symbol with `git grep` first. No phone is attached: nothing here is run on a device.

## Spec (binding)

**P1 (OMT Camera only), keyframe requests.** A receiver that joins or loses a frame asks for a keyframe; OMT
Camera never answers. Bread OMT Screen does, and is the reference. Read it with
`git -C F:\Coding\BreadMedia\openscreenshare-omthx show HEAD:<path>` (its commits 9757e6d and 5f35c62 added
this); never read its working tree, which may hold uncommitted work, and never change that repo from this run.
Bring over, adapted to OMT Camera's names:
- `libomt/cpp`: `omt_send_take_keyframe_request` and whatever it needs to see a receiver's request; and the
  `<OMTHX Seq=.. />` numbering of coded frames, if OMT Camera's sender does not number them yet (compare the
  two `libomt/cpp` trees first and list the differences in the ledger);
- `OMTBridge.cpp`: the JNI function; `OMTSender.java`: `takeKeyframeRequest()`;
- `OMTHXEncoder.java`: `requestKeyframe()` (`MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME`);
- `OMTStreamingManager.java`: after each coded frame is sent, take a request and pass it to the gate of P2.
Only what this needs: do not bring over unrelated changes (viewer lists, Wi-Fi pause handling) unless the
keyframe path cannot compile without them, and then record a `Ruling:`.

**P2, one forced keyframe per 200 ms.** A plain Java class with no Android import, next to the encoder:

```java
/** Lets a forced keyframe through at most once per MIN_GAP_MS. A request inside the gap is kept, not lost. */
final class KeyframeGate {
    static final long MIN_GAP_MS = 200;
    void request();                 // a receiver asked
    boolean take(long nowMs);       // true when the encoder should be asked now
}
```

`take` returns true when a request is waiting and at least `MIN_GAP_MS` passed since it last returned true;
it then clears the request. The place that polls `takeKeyframeRequest()` calls `request()` on true, then
`if (gate.take(SystemClock.elapsedRealtime())) enc.requestKeyframe();` on every coded frame. Bread OMT
Screen's "Recovered" counter counts keyframes actually forced.

**H1, colour.** In `OMTHXEncoder`, the `MediaFormat` also sets `KEY_COLOR_STANDARD = COLOR_STANDARD_BT709`,
`KEY_COLOR_RANGE = COLOR_RANGE_LIMITED`, `KEY_COLOR_TRANSFER = COLOR_TRANSFER_SDR_VIDEO`. Limited, not full:
every encoder supports it, and the Windows receiver now honours the range a stream signals. In
`OMTBridge.cpp`, the function that sends **coded** (HX) frames sets `frame.ColorSpace = OMTColorSpace_BT709` at
every size. The VMX (raw picture) path keeps its BT.601-below-720p rule.

**P3, the keyframe buffer.** `OMTHXEncoder` allocates a direct `ByteBuffer` for each keyframe to prepend the
headers. Keep one buffer and grow it when needed, but only if the callback's receiver has finished with the
data when it returns (read both the callback's contract and what the caller does with the buffer). If the data
is kept after the call returns, leave the allocation and record a `Ruling:`.

Anti-goals: no preview stream, no encoder name in the metadata, no UI change, no change to bitrates, no
change under `libvmx/`.

Tests (JVM unit tests, no device):
- `KeyframeGateTest`: no request gives false; a request gives true once; a second request 100 ms later gives
  false, and true at 200 ms; requests during the gap are one keyframe, not several.
- The existing unit tests pass.

Done when: `testDebugUnitTest` and `assembleDebug` pass. The report lists the owner's device checks: a
receiver joining mid-stream gets a picture at once (P1); black and white levels in an `omthx-recv --dump`
picture (H1).

## Tasks

### Task 1 (OMT Camera only): answer keyframe requests (P1)

Commit: `fix(omt): answer a receiver's keyframe request instead of waiting for the next one`.

### Task 2: the keyframe gate (P2)

TDD. Commit: `fix(omt): at most one forced keyframe per 200 ms`.

### Task 3: signal BT.709 limited range (H1)

Commit: `fix(omt): the HX stream says it is BT.709 limited range`.

### Task 4: one buffer for keyframe headers (P3)

Commit: `perf(omt): reuse the buffer that carries a keyframe's headers`. Or no commit and a `Ruling:`.

## Review Focus

- A keyframe request is never lost: one that arrives inside the gap is served when the gap ends.
- `requestKeyframe()` is called on a thread where the codec may be used, and not after `stop()`.
- The colour keys exist at the app's `minSdk` (they are API 24); guard them if `minSdk` is lower.
- A reused buffer is never read after the next frame overwrites it.
- OMT Camera run only: nothing was copied from Bread OMT Screen's working tree, and that repo is unchanged.
