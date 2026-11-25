# Private component qualification

GameCube native GX service, memory-bank callbacks, real hidden D3D12 draws/texture/display-copy/CPU rewrite checks; Wii bindings and gameplay are not qualified.

Normal package entry points select HLE; pinned native dependencies are explicit. Existing reference sources are retained behind an explicit accuracy selection. No consumer pins changed.

Baseline: `bc9f7b7a09a5b9a10a88b45cadc412225182d943`. Private evidence: campaign `beads-69le5`, `evidence/gc` and `evidence/gc-gpu`.
