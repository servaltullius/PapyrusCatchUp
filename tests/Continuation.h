#pragma once

#include "ResumeTrace.h"

namespace Continuation
{
    bool VerifyWorldSpace();
    bool IsStockWorldSpace(const RE::BSScript::IFunction& function);
    bool IsDynDOLODRoot(const RE::BSScript::StackFrame* frame);
    bool Install(ResumeTrace::Record* recorder, bool worldspaceOnly);
    void BeginTrial(bool enabled);
    void Arm(bool ready);
    void ObserveNative(const RE::BSScript::NF_util::NativeFunctionBase& function,
        const RE::BSScript::StackFrame& frame, bool succeeded);
    void ObserveResult(const RE::BSScript::NF_util::NativeFunctionBase& function,
        const RE::BSScript::StackFrame& frame, const RE::BSScript::Variable& self, const RE::BSScript::Variable& result);
    void ObserveReturn(RE::BSScript::Internal::CodeTasklet* tasklet);
}
