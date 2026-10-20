#include <juce_core/juce_core.h>

namespace
{
struct ConsoleLogger : juce::Logger
{
    void logMessage (const juce::String& message) override { std::cout << message << std::endl; }
};
} // namespace

int main()
{
    ConsoleLogger logger;
    juce::Logger::setCurrentLogger (&logger);

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    juce::Array<juce::UnitTest*> ours;
    for (auto* test : juce::UnitTest::getAllTests())
        if (test->getCategory() == "DSP" || test->getCategory() == "Model")
            ours.add (test);
    runner.runTests (ours);

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << " (" << failures << " failures)" << std::endl;
    juce::Logger::setCurrentLogger (nullptr);
    return failures == 0 ? 0 : 1;
}
