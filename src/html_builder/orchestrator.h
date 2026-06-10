#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

// Builds the full Home page (HTML or WML, depending on `epoch`) by
// assembling the container, menu, slider and home_content modules.
//
// Returns a malloc'd string, or NULL if any component could not be built
// (e.g. a template file is missing) - the caller should respond with a
// 500 Internal Server Error in that case. The caller must free() the
// returned buffer.
char *buildHomeWebSite(int epoch, const char *lang);

#endif // ORCHESTRATOR_H
