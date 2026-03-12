// Copyright (c) Project Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FDSMemProfilerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
#if DS_MEM_PROFILER_ENABLED
	TUniquePtr<class FDSMemMonitor>         Monitor;
	TUniquePtr<class FDSMemConsoleCommands> ConsoleCommands;
#endif
};
