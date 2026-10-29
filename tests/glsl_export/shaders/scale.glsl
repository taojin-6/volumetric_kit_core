// What vkc_glsl_exporter exports (../CMakeLists.txt), as
// "vkc_test/shaders/scale.glsl". ../../glsl_export_rebuild.cmake edits a copy's
// "2.0 * x".

float vkc_test_scale(float x) { return 2.0 * x; }
