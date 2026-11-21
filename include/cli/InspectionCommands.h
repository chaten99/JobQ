#pragma once

#include <iosfwd>

namespace jobq::cli {

class InteractiveRuntime;
struct RuntimeSnapshot;

void printRuntimeStatus(const RuntimeSnapshot& snapshot, std::ostream& output);
void printWorkers(const RuntimeSnapshot& snapshot, std::ostream& output);
void printQueue(const RuntimeSnapshot& snapshot, std::ostream& output);
void registerInspectionCommands(InteractiveRuntime& runtime);

}
