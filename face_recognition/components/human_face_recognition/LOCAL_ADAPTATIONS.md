# Local provenance and compatibility

Source: Espressif Component Registry `espressif/human_face_recognition` version 0.2.0.
Archive: `tools/human_face_recognition-0.2.0.zip` at project root.
Upstream commit recorded in the archive: `37c2aee306f334de6fe206320500185fcfdd2786`.
License: MIT, preserved in LICENSE.

Adaptations:

- Removed the obsolete `db_type_t` argument from HumanFaceRecognizer's constructor and called ESP-DL 3.1.3 `DataBase(db_path, 512)`.
- Replaced the component manifest dependency with an exact ESP-DL 3.1.3 dependency; this project's existing local human_face_detect component supplies detection.
- Model bytes and model preprocessing code are unchanged.

The application uses HumanFaceFeat directly, a dedicated NVS feature store, and its own cosine-similarity lookup so recognition and matching can be timed separately without modifying managed ESP-DL sources.
