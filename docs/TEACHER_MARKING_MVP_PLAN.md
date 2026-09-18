# Teacher Marking MVP

## Product outcome

Turn the Xournal++ fork into a desktop review workspace where a teacher can:

1. Open a student's PDF.
2. Import an AI-produced marking draft.
3. See numbered, colour-coded feedback grounded on the original work.
4. Jump from a feedback card to its exact page location.
5. edit the feedback, verdict, part mapping, and mark allocation.
6. Add feedback from a selected handwritten region.
7. Save the visual markup as `.xopp` and export the revised structured marking data.

The MVP must support both:

- humanities/debox marking, where an annotation may have an extracted-text anchor and an
  original-page bounding box;
- STEM marking, where atomic mark-scheme ticks use original-page bounding boxes.

## Product boundaries

### In the MVP

- A dedicated **Marking** sidebar inside Xournal++.
- A local, versioned marking manifest.
- Draft import and revised-draft export.
- Score and review progress at the top of the sidebar.
- Filters for all, correct, partial, incorrect, and unresolved feedback.
- Click-to-jump feedback cards.
- Editable comment, verdict, question-part ID, and awarded/max marks.
- Add-feedback-from-selection.
- PDF-native drawing, highlighting, text, undo/redo, layers, and `.xopp` persistence.
- A teacher-feedback layer whose visual boxes can be hidden for clean reading.
- Deterministic validation during review and export.
- Private local trials with one economics debox script and one STEM image-bbox script.

### Deferred

- Direct Fortify database writes or changes to Fortify.
- Running extraction or marking models inside the desktop binary.
- Authentication, class rosters, assignment distribution, and student submission intake.
- A hosted collaboration backend.
- Replacing `.xopp` with a new document engine.
- Full extracted-text reflow inside the canvas.

The AI boundary is deliberately local-file based for the first release. Existing marking
agents can produce a draft manifest; the desktop app is the human review and correction
surface. A later adapter can call the same agents or a Fortify-compatible service without
changing the review model.

## Why a native fork

Xournal++ already provides the difficult interaction layer: fast PDF rendering, pen and
highlighter tools, shape and text editing, selection, layers, undo/redo, autosave, PDF
export, and cross-platform packaging. The MVP should add marking semantics around those
capabilities instead of creating another PDF editor.

The principal extension seam is `Sidebar`: it owns a set of `AbstractSidebarPage`
implementations and already synchronises page navigation. A new marking page can remain
isolated from the drawing engine while using `Control`, `ScrollHandler`, `EditSelection`,
and normal Xournal elements.

## Core workflow

### 1. Prepare

The empty Marking sidebar asks for:

- student submission PDF;
- optional question paper;
- optional marking guide;
- subject mode: Humanities/Debox or STEM/Page boxes;
- a draft marking manifest, if one has already been generated.

For the MVP, the question paper and guide are references stored in the manifest. Draft
generation remains an external agent action.

### 2. Review

The sidebar header shows:

- assignment and student label;
- total awarded/max marks;
- reviewed count;
- validation state.

Feedback cards are ordered by page and then annotation number. A card shows:

- global annotation number;
- verdict colour and icon;
- question-part label;
- title and short comment;
- awarded/max marks when the annotation represents an atomic scoring tick;
- reviewed/unresolved state.

Selecting a card jumps to and frames its page box. Imported page boxes are materialised as
ordinary Xournal vector elements on a generated StudySzn feedback layer, so normal selection and
editing continue to work.

### 3. Correct

The teacher can:

- edit comment, verdict, part ID, and marks;
- move or resize a selected visual box;
- use the normal highlighter and text tools;
- select student evidence and create a new feedback card from that selection;
- mark a card reviewed;
- hide/show the feedback layer;
- undo drawing changes through Xournal++.

Cancelled work is never imported as active evidence. Humanities anchors are retained in the
manifest, but the original-page box is the editable desktop location.

### 4. Validate and export

Validation reports contract errors before a draft is exported:

- missing or invalid verdict;
- page outside the PDF;
- malformed or near-full-page box;
- awarded marks outside `[0, max]`;
- missing part ID;
- duplicate annotation ID;
- text and diagram anchors present together;
- unresolved feedback when final export is requested.

Known marking fields and anchors round-trip through the revised manifest. Unknown fields
are safely ignored in version 1. The `.xopp` remains the visual working document. The
structured export is the source for a later Fortify sync adapter.

## Marking manifest

The first implementation uses XML parsed with GLib's built-in GMarkup API, avoiding a new
runtime dependency. The file extension is `.xoppmark`.

```xml
<marking version="1">
  <assignment id="local-demo" title="Paper 1" student="Anonymous"
              mode="debox" source-pdf="submission.pdf"/>
  <score awarded="18" max="25"/>
  <parts>
    <part id="q2-a" label="Question 2(a)" awarded="8" max="10"
          rationale="Clear analysis; evaluation needs context."/>
  </parts>
  <annotations>
    <annotation id="a01" page="2" part-id="q2-a" verdict="partial"
                source="ai" severity="major" target-type="image"
                reviewed="false" awarded="0" max="0">
      <box y0="240" x0="120" y1="315" x1="850"/>
      <title>Complete the mechanism</title>
      <comment>Explain how the fall in cost changes supply before stating the price effect.</comment>
      <how-to-improve>Add the missing supply-chain step.</how-to-improve>
      <evidence>cost of production falls</evidence>
      <text-anchor block-id="p2-b4" exact="cost of production falls"/>
    </annotation>
  </annotations>
</marking>
```

Coordinates follow the existing marking contract:
`[y_min, x_min, y_max, x_max]`, integers from 0 to 1000, top-left origin.

## Architecture

### `marking/model`

Pure C++ data types:

- `MarkingDocument`
- `MarkingPart`
- `MarkingAnnotation`
- `MarkingBox`
- `MarkingVerdict`

The model owns validation and score aggregation. It has no GTK dependency and is unit
testable.

### `marking/io`

`MarkingXml` reads and writes `.xoppmark` files through GMarkup. Parsing is strict for the
version and coordinate contract, while unknown elements and attributes are ignored without
crashing so a newer producer does not make the app unusable.

### `gui/sidebar/marking`

`SidebarMarkingPage` implements `AbstractSidebarPage` and owns:

- assignment header;
- filter controls;
- annotation list;
- editable detail panel;
- import/export/add-from-selection actions;
- page navigation.

The sidebar never grades. It edits a draft supplied by a marking agent.

### Canvas bridge

The marking sidebar converts normalised boxes to page coordinates, creates or locates the
generated StudySzn feedback layer, materialises box elements, and reads changed geometry back for
export. Generated-layer replacement is registered as one grouped undo action; a production
iteration should move this bridge into a dedicated class.

### Future service adapter

A later `MarkingDraftProvider` interface can support:

- local command/agent;
- Fortify-compatible HTTPS endpoint;
- offline imported manifest.

No service credentials belong in `.xopp`, `.xoppmark`, logs, fixtures, or the public fork.

## Privacy and repository policy

- Real student PDFs and raw marking payloads are local trial data only.
- `.marking-local/` is gitignored.
- Public fixtures are synthetic and contain no names, handwriting, share tokens, signed
  URLs, storage paths, or database identifiers.
- No service-role keys or API tokens are stored in the fork.
- Trial import is read-only; MVP export writes a local revised manifest only.

## Trial plan

### Economics/debox

Use a recent finalized economics script that has:

- extracted text;
- one original-page `bbox_norm` for every annotation;
- both text/diagram anchors and page boxes;
- multiple verdicts and essay/CSQ parts.

Test:

1. Convert the private payload to `.xoppmark`.
2. Open its private source PDF.
3. Import all feedback.
4. Jump through first, middle, and last annotations.
5. Edit one comment and verdict.
6. Move one box and add one selected-region annotation.
7. Export and re-import.
8. Confirm score, order, anchors, and boxes round-trip.

### STEM/page boxes

Use a recent finalized physics script with atomic tick annotations.

Test:

1. Convert the private payload to `.xoppmark`.
2. Import the source PDF and feedback.
3. Check one correct, one partial, one incorrect, and one blank-target annotation.
4. Confirm every card maps to a distinct page box and part ID.
5. Change one atomic awarded mark and verify the total.
6. Export and re-import without creating text anchors.

## Milestones

### M0: Build baseline

- Fork and isolate a feature branch.
- Build unmodified upstream.
- Record macOS and Linux packaging constraints.

### M1: Data foundation

- Model, XML reader/writer, validation, and tests.
- Synthetic humanities and STEM fixtures.

### M2: Review UI

- Marking sidebar, score header, filters, cards, details editor, and navigation.

### M3: Canvas editing

- Materialise boxes and annotation numbers.
- Add from selection.
- Read moved geometry back on export.

### M4: Workflow trials

- Run private economics and STEM fixtures.
- Fix contract and usability failures.
- Independent code and UX review.

### M5: Distributable MVP

- CI builds and tests on supported platforms.
- macOS artifact or release.
- README with install, privacy, manifest, and workflow instructions.

## MVP acceptance criteria

- The upstream editor still opens, annotates, saves, and exports ordinary PDFs/XOPP files.
- A `.xoppmark` with at least 50 annotations loads without UI lock-up.
- Every imported card jumps to the correct page and region.
- Correct/partial/incorrect states are visually distinct but not colour-only.
- A teacher can change feedback and marks without editing XML.
- A selected region can become a new annotation.
- Exported data passes validation and round-trips without losing anchors.
- Both the private economics/debox and STEM/page-box trials pass.
- No real student data or credentials are present in git history or build artifacts.
- Fortify and all other repositories remain unchanged.
