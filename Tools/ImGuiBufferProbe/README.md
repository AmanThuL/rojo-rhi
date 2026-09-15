# ImGui geometry buffer regression

On Metal 4 Apple Silicon, after building App/ImGui:

```sh
python3 Tools/ImGuiBufferProbe/run.py --old-policy-control
```

The standalone Objective-C++ executable includes the actual fetched/patched backend and links
the existing `libImGui.a`. It does not change the Tests target or rebuild App. Compilation happens
in a temporary directory; Metal validation is enabled for each run.

Four frames visit slots 0, 1, 2, 0. Each frame encodes two different UI draw lists before submitting
either upload. A GPU event holds the first three frames pending until the slot-zero revisit.
The probe observes real buffer checkouts independently of the backend cache containers and checks:

- The second draw list preserves the first list's vertex and index bytes.
- Every pending upload has a different buffer identity, including uploads in other paced slots.
- Revisiting slot zero waits for its pending platform retirement event, then reuses its old storage.
- After queue-wide retirement, all eight offscreen outputs retain their expected red/green geometry.

The optional control changes only the end-of-draw return destination in a temporary backend copy
back to the former immediately reusable cache. That executable must fail the upload and pixel checks;
the working tree is untouched. It demonstrates sensitivity to the original overwrite policy.

This probes buffer ownership and the platform event protocol without SDL or native windows. It
does not validate OS window positioning, drawable acquisition, occlusion, or visual frame pacing.
