// Once per second (every 60 scene updates): what the bodies are, written to <run>/census.csv.
// Answers gate G0's second question: how many awake bodies are free debris (no joints) and how big they are.
#pragma once
#include <string>

namespace census {

void Start(const std::string& runDir);
void OnTick();  // main thread, start of each scene update

}  // namespace census
