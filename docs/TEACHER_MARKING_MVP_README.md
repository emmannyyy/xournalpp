# StudySzn Marker MVP

This fork adds a teacher-review workspace to Xournal++. It imports structured AI
feedback, places editable boxes on the student's original PDF, and lets the teacher
revise comments, verdicts, question-part mapping, and marks before exporting a revised
`.xoppmark` manifest.

## What works

- Humanities/debox feedback with preserved text or diagram anchors.
- STEM page-box feedback with atomic marks linked to part totals.
- A dedicated Marking sidebar with score, review progress, filters, navigation, and
  editable feedback details.
- Colour- and line-style-coded visual boxes on a generated StudySzn feedback layer.
- Add feedback from the current selection.
- Move or resize feedback boxes using Xournal++ selection tools and synchronise the
  geometry when exporting.
- Continue using normal pen, highlighter, text, undo/redo, `.xopp`, and PDF export tools.

AI marking generation and Fortify writes are intentionally outside this local MVP. Existing
marking workflows produce the input manifest; this application is the human correction
surface.

## Safe development launch

The fork uses:

- application ID `com.studyszn.marker`;
- config folder `studyszn-marker`;
- macOS bundle ID `com.studyszn.marker`;
- an isolated `.marking-local/runtime` XDG sandbox.

It does not migrate settings from Xournal++, and its macOS metadata does not register as a
handler for `.xopp`, `.xoj`, or `.xopt`.

Build and launch:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_AUDIO=off \
  -DENABLE_FLOAT_FROM_CHARS=off \
  -DENABLE_GTEST=on \
  -DDOWNLOAD_GTEST=on
cmake --build build
cmake --install build --prefix "$PWD/.install"
scripts/teacher-marking/run-dev-sandbox.sh
```

Do not launch the development binary directly while validating isolation; use the sandbox
script.

## Secret-free demo

Generate a synthetic two-page PDF and both workflow manifests:

```bash
python3 scripts/teacher-marking/create_demo.py
```

The files are written under `.marking-local/demo/`:

- `submission.pdf`
- `humanities-debox.xoppmark`
- `stem-page-boxes.xoppmark`

Open `submission.pdf`, then use **Import draft** in the Marking sidebar. A manifest may
also be opened directly from the command line.

## Teacher workflow

1. Open the student's source PDF.
2. Import the matching `.xoppmark` draft.
3. Select a feedback card to jump to its page and reveal its details.
4. Edit the verdict, part ID, marks, comment, or improvement guidance, then click
   **Apply changes**.
5. Use the normal selection tool to move or resize a feedback box.
6. Select a region and click **Add from selection** to create teacher-authored feedback.
7. Save the visual document as `.xopp`.
8. Click **Export revised** to synchronise geometry, validate the draft, and save the
   structured result.

## Private Fortify trial

`export_fortify_fixture.py` performs a read-only export into `.marking-local/`. It never
modifies Fortify:

```bash
FORTIFY_ENV_FILE=/path/to/fortify/.env \
  python3 scripts/teacher-marking/export_fortify_fixture.py \
  SCRIPT_UUID .marking-local/my-trial
```

For STEM/page-box exports, also pass `--mode page-boxes
--cancelled-work-excluded` after checking that the producer excluded cancelled work.

Real student PDFs and payloads must remain under `.marking-local/`, which is gitignored.
Never add them to tests, screenshots, releases, issues, or commits.

## Validation

```bash
CI=true ./build/test/test-units
XOPP_MARKING_TRIAL_ROOT="$PWD/.marking-local" \
  CI=true ./build/test/test-units --gtest_filter='Marking*'
```

Version 1 reports malformed boxes, invalid page/mark ranges, unknown parts, duplicate IDs
or near-duplicate positions, unresolved verdicts, invalid annotation sources, and
conflicting text/diagram anchors.

## MVP limitations

- The desktop app imports drafts; it does not invoke an AI model itself.
- Export remains local and does not sync back to Fortify.
- Structured feedback is stored beside the `.xopp`; it is not embedded inside it.
- Re-importing a manifest deterministically rebuilds the generated feedback layer.
- Only the macOS development build has been exercised in this prototype.
