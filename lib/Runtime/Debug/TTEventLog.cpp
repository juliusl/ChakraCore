//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeDebugPch.h"

#if ENABLE_TTD

namespace TTD
{
    TTDExceptionFramePopper::TTDExceptionFramePopper()
        : m_log(nullptr), m_function(nullptr)
    {
        ;
    }

    TTDExceptionFramePopper::~TTDExceptionFramePopper()
    {
#if ENABLE_TTD_DEBUGGING
        //we didn't clear this so an exception was thrown and we are propagating
        if(this->m_log != nullptr)
        {
            //if it doesn't have an exception frame then this is the frame where the exception was thrown so record our info
            this->m_log->PopCallEventException(this->m_function, !this->m_log->HasImmediateExceptionFrame());
        }
#endif
    }

    void TTDExceptionFramePopper::PushInfo(EventLog* log, Js::JavascriptFunction* function)
    {
        this->m_log = log; //set the log info so if the pop isn't called the destructor will record propagation
        this->m_function = function;
    }

    void TTDExceptionFramePopper::PopInfo()
    {
        this->m_log = nullptr; //normal pop (no exception) just clear so destructor nops
    }

    TTDRecordExternalFunctionCallActionPopper::TTDRecordExternalFunctionCallActionPopper(Js::JavascriptFunction* function, NSLogEvents::EventLogEntry* callAction)
        : m_function(function), m_callAction(callAction)
    {
        this->m_function->GetScriptContext()->TTDRootNestingCount++;
    }

    TTDRecordExternalFunctionCallActionPopper::~TTDRecordExternalFunctionCallActionPopper()
    {
        if(this->m_callAction != nullptr)
        {
            this->m_function->GetScriptContext()->GetThreadContext()->TTDLog->RecordExternalCallEvent_Complete(this->m_callAction, this->m_function, false, false, nullptr);

            this->m_callAction = nullptr;
            this->m_function->GetScriptContext()->TTDRootNestingCount--;
        }
    }

    void TTDRecordExternalFunctionCallActionPopper::NormalReturn(bool checkException, Js::Var returnValue)
    {
        AssertMsg(this->m_callAction != nullptr, "Should never be null on normal return!");

        this->m_function->GetScriptContext()->GetThreadContext()->TTDLog->RecordExternalCallEvent_Complete(this->m_callAction, this->m_function, true, checkException, returnValue);

        this->m_callAction = nullptr;
        this->m_function->GetScriptContext()->TTDRootNestingCount--;
    }

    TTDReplayExternalFunctionCallActionPopper::TTDReplayExternalFunctionCallActionPopper(Js::JavascriptFunction* function)
        : m_function(function)
    {
        this->m_function->GetScriptContext()->TTDRootNestingCount++;
    }

    TTDReplayExternalFunctionCallActionPopper::~TTDReplayExternalFunctionCallActionPopper()
    {
        this->m_function->GetScriptContext()->TTDRootNestingCount--;
    }

    TTDRecordJsRTFunctionCallActionPopper::TTDRecordJsRTFunctionCallActionPopper(Js::ScriptContext* ctx, NSLogEvents::EventLogEntry* callAction)
        : m_ctx(ctx), m_callAction(callAction)
    {
        ;
    }

    TTDRecordJsRTFunctionCallActionPopper::~TTDRecordJsRTFunctionCallActionPopper()
    {
        if(this->m_callAction != nullptr)
        {
            //
            //TODO: we will want to be a bit more detailed on this later
            //
            bool hasScriptException = true; 
            bool hasTerminalException = false;

            NSLogEvents::JsRTCallFunctionAction_ProcessReturn(this->m_callAction, nullptr, hasScriptException, hasTerminalException);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
            NSLogEvents::JsRTCallFunctionAction_ProcessDiagInfoPost(this->m_callAction, this->m_ctx->GetThreadContext()->TTDLog->GetCurrentWallTime(), this->m_ctx->GetThreadContext()->TTDLog->GetLastEventTime());
#endif

            this->m_callAction = nullptr;
            this->m_ctx->TTDRootNestingCount--;
        }
    }

    void TTDRecordJsRTFunctionCallActionPopper::NormalReturn(Js::Var returnValue)
    {
        AssertMsg(this->m_callAction != nullptr, "Should never be null on normal return!");

        //
        //TODO: we will want to be a bit more detailed on this later
        //
        bool hasScriptException = false;
        bool hasTerminalException = false;

        NSLogEvents::JsRTCallFunctionAction_ProcessReturn(this->m_callAction, returnValue, hasScriptException, hasTerminalException);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        NSLogEvents::JsRTCallFunctionAction_ProcessDiagInfoPost(this->m_callAction, this->m_ctx->GetThreadContext()->TTDLog->GetCurrentWallTime(), this->m_ctx->GetThreadContext()->TTDLog->GetLastEventTime());
#endif

        this->m_callAction = nullptr;
    }

    /////////////

    void TTEventList::AddArrayLink()
    {
        TTEventListLink* newHeadBlock = this->m_alloc->SlabAllocateStruct<TTEventListLink>();
        newHeadBlock->BlockData = this->m_alloc->SlabAllocateFixedSizeArray<NSLogEvents::EventLogEntry, TTD_EVENTLOG_LIST_BLOCK_SIZE>();
        memset(newHeadBlock->BlockData, 0, TTD_EVENTLOG_LIST_BLOCK_SIZE * sizeof(NSLogEvents::EventLogEntry));

        newHeadBlock->CurrPos = 0;
        newHeadBlock->StartPos = 0;

        newHeadBlock->Next = nullptr;
        newHeadBlock->Previous = this->m_headBlock;

        if(this->m_headBlock != nullptr)
        {
            this->m_headBlock->Next = newHeadBlock;
        }

        this->m_headBlock = newHeadBlock;
    }

    void TTEventList::RemoveArrayLink(TTEventListLink* block)
    {
        AssertMsg(block->Previous == nullptr, "Not first event block in log!!!");
        AssertMsg(block->StartPos == block->CurrPos, "Haven't cleared all the events in this link");

        if(block->Next == nullptr)
        {
            this->m_headBlock = nullptr; //was only 1 block to we are now all null
        }
        else
        {
            block->Next->Previous = nullptr;
        }

        this->m_alloc->UnlinkAllocation(block->BlockData);
        this->m_alloc->UnlinkAllocation(block);
    }

    TTEventList::TTEventList(UnlinkableSlabAllocator* alloc)
        : m_alloc(alloc), m_headBlock(nullptr)
    {
        ;
    }

    void TTEventList::UnloadEventList(NSLogEvents::EventLogEntryVTableEntry* vtable)
    {
        if(this->m_headBlock == nullptr)
        {
            return;
        }

        TTEventListLink* firstBlock = this->m_headBlock;
        while(firstBlock->Previous != nullptr)
        {
            firstBlock = firstBlock->Previous;
        }

        TTEventListLink* curr = firstBlock;
        while(curr != nullptr)
        {
            for(uint32 i = curr->StartPos; i < curr->CurrPos; ++i)
            {
                const NSLogEvents::EventLogEntry* entry = curr->BlockData + i;
                auto unloadFP = vtable[(uint32)entry->EventKind].UnloadFP; //use vtable magic here

                if(unloadFP != nullptr)
                {
                    unloadFP(curr->BlockData + i, *(this->m_alloc));
                }
            }
            curr->StartPos = curr->CurrPos;

            TTEventListLink* next = curr->Next;
            this->RemoveArrayLink(curr);
            curr = next;
        }

        this->m_headBlock = nullptr;
    }

    NSLogEvents::EventLogEntry* TTEventList::GetNextAvailableEntry()
    {
        if((this->m_headBlock == nullptr) || (this->m_headBlock->CurrPos == TTD_EVENTLOG_LIST_BLOCK_SIZE))
        {
            this->AddArrayLink();
        }

        NSLogEvents::EventLogEntry* entry = (this->m_headBlock->BlockData + this->m_headBlock->CurrPos);
        this->m_headBlock->CurrPos++;

        return entry;
    }

    void TTEventList::DeleteFirstEntry(TTEventListLink* block, NSLogEvents::EventLogEntry* data, NSLogEvents::EventLogEntryVTableEntry* vtable)
    {
        AssertMsg(block->Previous == nullptr, "Not first event block in log!!!");
        AssertMsg((block->BlockData + block->StartPos) == data, "Not the data at the start of the list!!!");

        auto unloadFP = vtable[(uint32)data->EventKind].UnloadFP; //use vtable magic here

        if(unloadFP != nullptr)
        {
            unloadFP(data, *(this->m_alloc));
        }

        block->StartPos++;
        if(block->StartPos == block->CurrPos)
        {
            this->RemoveArrayLink(block);
        }
    }

    bool TTEventList::IsEmpty() const
    {
        return this->m_headBlock == nullptr;
    }

    uint32 TTEventList::Count() const
    {
        uint32 count = 0;

        for(TTEventListLink* curr = this->m_headBlock; curr != nullptr; curr = curr->Previous)
        {
            count += (curr->CurrPos - curr->StartPos);
        }

        return (uint32)count;
    }

    TTEventList::Iterator::Iterator()
        : m_currLink(nullptr), m_currIdx(0)
    {
        ;
    }

    TTEventList::Iterator::Iterator(TTEventListLink* head, uint32 pos)
        : m_currLink(head), m_currIdx(pos)
    {
        ;
    }

    const NSLogEvents::EventLogEntry* TTEventList::Iterator::Current() const
    {
        AssertMsg(this->IsValid(), "Iterator is invalid!!!");

        return (this->m_currLink->BlockData + this->m_currIdx);
    }

    NSLogEvents::EventLogEntry* TTEventList::Iterator::Current()
    {
        AssertMsg(this->IsValid(), "Iterator is invalid!!!");

        return (this->m_currLink->BlockData + this->m_currIdx);
    }

    bool TTEventList::Iterator::IsValid() const
    {
        return (this->m_currLink != nullptr && this->m_currLink->StartPos <= this->m_currIdx && this->m_currIdx < this->m_currLink->CurrPos);
    }

    void TTEventList::Iterator::MoveNext()
    {
        if(this->m_currIdx < (this->m_currLink->CurrPos - 1))
        {
            this->m_currIdx++;
        }
        else
        {
            this->m_currLink = this->m_currLink->Next;
            this->m_currIdx = (this->m_currLink != nullptr) ? this->m_currLink->StartPos : 0;
        }
    }

    void TTEventList::Iterator::MovePrevious()
    {
        if(this->m_currIdx > this->m_currLink->StartPos)
        {
            this->m_currIdx--;
        }
        else
        {
            this->m_currLink = this->m_currLink->Previous;
            this->m_currIdx = (this->m_currLink != nullptr) ? (this->m_currLink->CurrPos - 1) : 0;
        }
    }

    TTEventList::Iterator TTEventList::GetIteratorAtFirst() const
    {
        if(this->m_headBlock == nullptr)
        {
            return Iterator(nullptr, 0);
        }
        else
        {
            TTEventListLink* firstBlock = this->m_headBlock;
            while(firstBlock->Previous != nullptr)
            {
                firstBlock = firstBlock->Previous;
            }

            return Iterator(firstBlock, firstBlock->StartPos);
        }
    }

    TTEventList::Iterator TTEventList::GetIteratorAtLast() const
    {
        if(this->m_headBlock == nullptr)
        {
            return Iterator(nullptr, 0);
        }
        else
        {
            return Iterator(this->m_headBlock, this->m_headBlock->CurrPos - 1);
        }
    }

    //////

    const SingleCallCounter& EventLog::GetTopCallCounter() const
    {
        AssertMsg(this->m_callStack.Count() != 0, "Empty stack!");

        return this->m_callStack.Item(this->m_callStack.Count() - 1);
    }

    SingleCallCounter& EventLog::GetTopCallCounter()
    {
        AssertMsg(this->m_callStack.Count() != 0, "Empty stack!");

        return this->m_callStack.Item(this->m_callStack.Count() - 1);
    }

    const SingleCallCounter& EventLog::GetTopCallCallerCounter() const
    {
        AssertMsg(this->m_callStack.Count() >= 2, "Empty stack!");

        return this->m_callStack.Item(this->m_callStack.Count() - 2);
    }

    int64 EventLog::GetCurrentEventTimeAndAdvance()
    {
        return this->m_eventTimeCtr++;
    }

    void EventLog::AdvanceTimeAndPositionForReplay()
    {
        this->m_eventTimeCtr++;
        this->m_currentReplayEventIterator.MoveNext();

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        AssertMsg(!this->m_currentReplayEventIterator.IsValid() || this->m_eventTimeCtr == this->m_currentReplayEventIterator.Current()->EventTimeStamp, "Something is out of sync.");
#endif
    }

    void EventLog::UpdateComputedMode()
    {
        AssertMsg(this->m_modeStack.Count() >= 0, "Should never be empty!!!");

        TTDMode cm = TTDMode::Invalid;
        for(int32 i = 0; i < this->m_modeStack.Count(); ++i)
        {
            TTDMode m = this->m_modeStack.Item(i);
            switch(m)
            {
            case TTDMode::Pending:
            case TTDMode::Detached:
            case TTDMode::RecordEnabled:
            case TTDMode::DebuggingEnabled:
                AssertMsg(i == 0, "One of these should always be first on the stack.");
                cm = m;
                break;
            case TTDMode::ExcludedExecution:
                AssertMsg(i != 0, "A base mode should always be first on the stack.");
                cm |= m;
                break;
            default:
                AssertMsg(false, "This mode is unknown or should never appear.");
                break;
            }
        }

        this->m_currentMode = cm;

        if(this->m_ttdContext != nullptr)
        {
            this->m_ttdContext->TTDMode = this->m_currentMode;
        }
    }

    void EventLog::UnloadRetainedData()
    {
        if(this->m_lastInflateMap != nullptr)
        {
            HeapDelete(this->m_lastInflateMap);
            this->m_lastInflateMap = nullptr;
        }

        if(this->m_propertyRecordPinSet != nullptr)
        {
            this->m_propertyRecordPinSet->GetAllocator()->RootRelease(this->m_propertyRecordPinSet);
            this->m_propertyRecordPinSet = nullptr;
        }
    }

    SnapShot* EventLog::DoSnapshotExtract_Helper()
    {
        AssertMsg(this->m_ttdContext != nullptr, "We aren't actually tracking anything!!!");

        JsUtil::List<Js::Var, HeapAllocator> roots(&HeapAllocator::Instance);
        JsUtil::List<Js::ScriptContext*, HeapAllocator> ctxs(&HeapAllocator::Instance);

        ctxs.Add(this->m_ttdContext);
        this->m_ttdContext->TTDContextInfo->ExtractSnapshotRoots(roots);

        this->m_snapExtractor.BeginSnapshot(this->m_threadContext, roots, ctxs);
        this->m_snapExtractor.DoMarkWalk(roots, ctxs, this->m_threadContext);

        ///////////////////////////
        //Phase 2: Evacuate marked objects
        //Allows for parallel execute and evacuate (in conjunction with later refactoring)

        this->m_snapExtractor.EvacuateMarkedIntoSnapshot(this->m_threadContext, ctxs);

        ///////////////////////////
        //Phase 3: Complete and return snapshot

        return this->m_snapExtractor.CompleteSnapshot();
    }

    void EventLog::ReplaySnapshotEvent()
    {
#if ENABLE_SNAPSHOT_COMPARE
        BEGIN_ENTER_SCRIPT(this->m_ttdContext, true, true, true);
        {
            //this->m_threadContext->TTDLog->PushMode(TTD::TTDMode::ExcludedExecution);

            NSLogEvents::EventLogEntry* evt = this->m_currentReplayEventIterator.Current();
            NSLogEvents::SnapshotEventLogEntry_EnsureSnapshotDeserialized(evt, this->m_logInfoRootDir.Contents, this->m_threadContext);

            SnapShot* snap = this->DoSnapshotExtract_Helper();

            const NSLogEvents::SnapshotEventLogEntry* recordedSnapEntry = NSLogEvents::GetInlineEventDataAs<NSLogEvents::SnapshotEventLogEntry, NSLogEvents::EventKind::SnapshotTag>(evt);
            const SnapShot* recordedSnap = recordedSnapEntry->Snap;

            TTDCompareMap compareMap(this->m_threadContext);
            SnapShot::InitializeForSnapshotCompare(recordedSnap, snap, compareMap);
            SnapShot::DoSnapshotCompare(recordedSnap, snap, compareMap);

            HeapDelete(snap);

            //this->m_threadContext->TTDLog->PopMode(TTD::TTDMode::ExcludedExecution);
        }
        END_ENTER_SCRIPT;
#endif


#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteLiteralMsg("---SNAPSHOT EVENT---\n");
#endif

        this->AdvanceTimeAndPositionForReplay(); //move along
    }

    void EventLog::ReplayEventLoopYieldPointEvent()
    {
        this->m_ttdContext->TTDContextInfo->ClearLocalRootsAndRefreshMap();

        this->AdvanceTimeAndPositionForReplay(); //move along
    }

    void EventLog::AbortReplayReturnToHost()
    {
        throw TTDebuggerAbortException::CreateAbortEndOfLog(_u("End of log reached -- returning to top-level."));
    }

    void EventLog::InitializeEventListVTable()
    {
        this->m_eventListVTable = this->m_miscSlabAllocator.SlabAllocateArray<NSLogEvents::EventLogEntryVTableEntry>((uint32)NSLogEvents::EventKind::Count);

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::SnapshotTag] = {nullptr, NSLogEvents::SnapshotEventLogEntry_UnloadEventMemory, NSLogEvents::SnapshotEventLogEntry_Emit, NSLogEvents::SnapshotEventLogEntry_Parse};
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::EventLoopYieldPointTag] = { nullptr, nullptr, NSLogEvents::EventLoopYieldPointEntry_Emit, NSLogEvents::EventLoopYieldPointEntry_Parse};
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::TopLevelCodeTag] = { nullptr, nullptr, NSLogEvents::CodeLoadEventLogEntry_Emit, NSLogEvents::CodeLoadEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::TelemetryLogTag] = { nullptr, NSLogEvents::TelemetryEventLogEntry_UnloadEventMemory, NSLogEvents::TelemetryEventLogEntry_Emit, NSLogEvents::TelemetryEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::DoubleTag] = { nullptr, nullptr, NSLogEvents::DoubleEventLogEntry_Emit, NSLogEvents::DoubleEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::StringTag] = { nullptr, NSLogEvents::StringValueEventLogEntry_UnloadEventMemory, NSLogEvents::StringValueEventLogEntry_Emit, NSLogEvents::StringValueEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::RandomSeedTag] = { nullptr, nullptr, NSLogEvents::RandomSeedEventLogEntry_Emit, NSLogEvents::RandomSeedEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::PropertyEnumTag] = { nullptr, NSLogEvents::PropertyEnumStepEventLogEntry_UnloadEventMemory, NSLogEvents::PropertyEnumStepEventLogEntry_Emit, NSLogEvents::PropertyEnumStepEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::SymbolCreationTag] = { nullptr, nullptr, NSLogEvents::SymbolCreationEventLogEntry_Emit, NSLogEvents::SymbolCreationEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::ExternalCbRegisterCall] = { nullptr, nullptr, NSLogEvents::ExternalCbRegisterCallEventLogEntry_Emit, NSLogEvents::ExternalCbRegisterCallEventLogEntry_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::ExternalCallTag] = { nullptr, NSLogEvents::ExternalCallEventLogEntry_UnloadEventMemory, NSLogEvents::ExternalCallEventLogEntry_Emit, NSLogEvents::ExternalCallEventLogEntry_Parse };

#if !INT32VAR
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CreateIntegerActionTag] = { NSLogEvents::CreateInt_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::CreateIntegerActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::CreateIntegerActionTag> };
#endif

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CreateNumberActionTag] = { NSLogEvents::CreateNumber_Execute, nullptr, NSLogEvents::JsRTDoubleArgumentAction_Emit<NSLogEvents::EventKind::CreateNumberActionTag>, NSLogEvents::JsRTDoubleArgumentAction_Parse<NSLogEvents::EventKind::CreateNumberActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CreateBooleanActionTag] = { NSLogEvents::CreateBoolean_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::CreateBooleanActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::CreateBooleanActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CreateStringActionTag] = { NSLogEvents::CreateString_Execute, NSLogEvents::JsRTStringArgumentAction_UnloadEventMemory<NSLogEvents::EventKind::CreateStringActionTag>, NSLogEvents::JsRTStringArgumentAction_Emit<NSLogEvents::EventKind::CreateStringActionTag>, NSLogEvents::JsRTStringArgumentAction_Parse<NSLogEvents::EventKind::CreateStringActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CreateSymbolActionTag] = { NSLogEvents::CreateSymbol_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::CreateSymbolActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::CreateSymbolActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::VarConvertToNumberActionTag] = { NSLogEvents::VarConvertToNumber_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::VarConvertToNumberActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::VarConvertToNumberActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::VarConvertToBooleanActionTag] = { NSLogEvents::VarConvertToBoolean_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::VarConvertToBooleanActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::VarConvertToBooleanActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::VarConvertToStringActionTag] = { NSLogEvents::VarConvertToString_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::VarConvertToStringActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::VarConvertToStringActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::VarConvertToObjectActionTag] = { NSLogEvents::VarConvertToObject_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::VarConvertToObjectActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::VarConvertToObjectActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AddRootRefActionTag] = { NSLogEvents::AddRootRef_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::AddRootRefActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::AddRootRefActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::RemoveRootRefActionTag] = { NSLogEvents::RemoveRootRef_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::RemoveRootRefActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::RemoveRootRefActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateObjectActionTag] = { NSLogEvents::AllocateObject_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::AllocateObjectActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::AllocateObjectActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateExternalObjectActionTag] = { NSLogEvents::AllocateExternalObject_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::AllocateExternalObjectActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::AllocateExternalObjectActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateArrayActionTag] = { NSLogEvents::AllocateArrayAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::AllocateArrayActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::AllocateArrayActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateArrayBufferActionTag] = { NSLogEvents::AllocateArrayBufferAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::AllocateArrayBufferActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::AllocateArrayBufferActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag] = { NSLogEvents::AllocateExternalArrayBufferAction_Execute, NSLogEvents::JsRTByteBufferAction_UnloadEventMemory<NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag>, NSLogEvents::JsRTByteBufferAction_Emit<NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag>, NSLogEvents::JsRTByteBufferAction_Parse<NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::AllocateFunctionActionTag] = { NSLogEvents::AllocateFunctionAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetAndClearExceptionActionTag] = { NSLogEvents::GetAndClearExceptionAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::GetAndClearExceptionActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::GetAndClearExceptionActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetPropertyActionTag] = { NSLogEvents::GetPropertyAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::GetPropertyActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::GetPropertyActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetIndexActionTag] = { NSLogEvents::GetIndexAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::GetIndexActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::GetIndexActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetOwnPropertyInfoActionTag] = { NSLogEvents::GetOwnPropertyInfoAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::GetOwnPropertyInfoActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::GetOwnPropertyInfoActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetOwnPropertyNamesInfoActionTag] = { NSLogEvents::GetOwnPropertyNamesInfoAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::GetOwnPropertyNamesInfoActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::GetOwnPropertyNamesInfoActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetOwnPropertySymbolsInfoActionTag] = { NSLogEvents::GetOwnPropertySymbolsInfoAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::GetOwnPropertySymbolsInfoActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::GetOwnPropertySymbolsInfoActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::DefinePropertyActionTag] = { NSLogEvents::DefinePropertyAction_Execute, nullptr, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Emit<NSLogEvents::EventKind::DefinePropertyActionTag>, NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction_Parse<NSLogEvents::EventKind::DefinePropertyActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::DeletePropertyActionTag] = { NSLogEvents::DeletePropertyAction_Execute, nullptr, NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction_Emit<NSLogEvents::EventKind::DeletePropertyActionTag>, NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction_Parse<NSLogEvents::EventKind::DeletePropertyActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::SetPrototypeActionTag] = { NSLogEvents::SetPrototypeAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::SetPrototypeActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::SetPrototypeActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::SetPropertyActionTag] = { NSLogEvents::SetPropertyAction_Execute, nullptr, NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction_Emit<NSLogEvents::EventKind::SetPropertyActionTag>, NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction_Parse<NSLogEvents::EventKind::SetPropertyActionTag> };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::SetIndexActionTag] = { NSLogEvents::SetIndexAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::SetIndexActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::SetIndexActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::GetTypedArrayInfoActionTag] = { NSLogEvents::GetTypedArrayInfoAction_Execute, nullptr, NSLogEvents::JsRTVarsArgumentAction_Emit<NSLogEvents::EventKind::GetTypedArrayInfoActionTag>, NSLogEvents::JsRTVarsArgumentAction_Parse<NSLogEvents::EventKind::GetTypedArrayInfoActionTag> };

        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::ConstructCallActionTag] = { NSLogEvents::JsRTConstructCallAction_Execute, NSLogEvents::JsRTConstructCallAction_UnloadEventMemory, NSLogEvents::JsRTConstructCallAction_Emit, NSLogEvents::JsRTConstructCallAction_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CallbackOpActionTag] = { NSLogEvents::JsRTCallbackAction_Execute, NSLogEvents::JsRTCallbackAction_UnloadEventMemory, NSLogEvents::JsRTCallbackAction_Emit, NSLogEvents::JsRTCallbackAction_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CodeParseActionTag] = { NSLogEvents::JsRTCodeParseAction_Execute, NSLogEvents::JsRTCodeParseAction_UnloadEventMemory, NSLogEvents::JsRTCodeParseAction_Emit, NSLogEvents::JsRTCodeParseAction_Parse };
        this->m_eventListVTable[(uint32)NSLogEvents::EventKind::CallExistingFunctionActionTag] = { NSLogEvents::JsRTCallFunctionAction_Execute, NSLogEvents::JsRTCallFunctionAction_UnloadEventMemory, NSLogEvents::JsRTCallFunctionAction_Emit, NSLogEvents::JsRTCallFunctionAction_Parse };
    }

    EventLog::EventLog(ThreadContext* threadContext, LPCWSTR logDir, uint32 snapInterval, uint32 snapHistoryLength)
        : m_threadContext(threadContext), m_eventSlabAllocator(TTD_SLAB_BLOCK_ALLOCATION_SIZE_MID), m_miscSlabAllocator(TTD_SLAB_BLOCK_ALLOCATION_SIZE_SMALL), m_snapInterval(snapInterval), m_snapHistoryLength(snapHistoryLength),
        m_eventTimeCtr(0), m_timer(), m_runningFunctionTimeCtr(0), m_topLevelCallbackEventTime(-1), m_hostCallbackId(-1),
        m_eventList(&this->m_eventSlabAllocator), m_eventListVTable(nullptr), m_currentReplayEventIterator(),
        m_callStack(&HeapAllocator::Instance, 32), 
#if ENABLE_TTD_DEBUGGING
        m_isReturnFrame(false), m_isExceptionFrame(false), m_lastFrame(), m_pendingTTDBP(), m_activeBPId(-1), m_activeTTDBP(),
#endif
#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        m_diagnosticLogger(),
#endif
        m_modeStack(&HeapAllocator::Instance), m_currentMode(TTDMode::Pending),
        m_ttdContext(nullptr),
        m_snapExtractor(), m_elapsedExecutionTimeSinceSnapshot(0.0),
        m_lastInflateSnapshotTime(-1), m_lastInflateMap(nullptr), m_propertyRecordPinSet(nullptr), m_propertyRecordList(&this->m_miscSlabAllocator), 
        m_loadedTopLevelScripts(&this->m_miscSlabAllocator), m_newFunctionTopLevelScripts(&this->m_miscSlabAllocator), m_evalTopLevelScripts(&this->m_miscSlabAllocator)
    {
        JsSupport::CopyStringToHeapAllocator(logDir, this->m_logInfoRootDir);

        this->InitializeEventListVTable();

        if(this->m_snapHistoryLength < 2)
        {
            this->m_snapHistoryLength = 2;
        }

        this->m_modeStack.Add(TTDMode::Pending);

        this->m_propertyRecordPinSet = RecyclerNew(threadContext->GetRecycler(), PropertyRecordPinSet, threadContext->GetRecycler());
        this->m_threadContext->GetRecycler()->RootAddRef(this->m_propertyRecordPinSet);
    }

    EventLog::~EventLog()
    {
        this->m_eventList.UnloadEventList(this->m_eventListVTable);

        this->UnloadRetainedData();

        JsSupport::DeleteStringFromHeapAllocator(this->m_logInfoRootDir);
    }

    void EventLog::InitForTTDRecord()
    {
        //Prepare the logging stream so it is ready for us to write into
        this->m_threadContext->TTDWriteInitializeFunction(this->m_logInfoRootDir.Contents);

        //pin all the current properties so they don't move/disappear on us
        for(Js::PropertyId pid = TotalNumberOfBuiltInProperties + 1; pid < this->m_threadContext->GetMaxPropertyId(); ++pid)
        {
            const Js::PropertyRecord* pRecord = this->m_threadContext->GetPropertyName(pid);
            this->AddPropertyRecord(pRecord);
        }
    }

    void EventLog::InitForTTDReplay()
    {
        this->ParseLogInto();

        Js::PropertyId maxPid = TotalNumberOfBuiltInProperties + 1;
        JsUtil::BaseDictionary<Js::PropertyId, NSSnapType::SnapPropertyRecord*, HeapAllocator> pidMap(&HeapAllocator::Instance);

        for(auto iter = this->m_propertyRecordList.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            maxPid = max(maxPid, iter.Current()->PropertyId);
            pidMap.AddNew(iter.Current()->PropertyId, iter.Current());
        }

        for(Js::PropertyId cpid = TotalNumberOfBuiltInProperties + 1; cpid <= maxPid; ++cpid)
        {
            NSSnapType::SnapPropertyRecord* spRecord = pidMap.LookupWithKey(cpid, nullptr);
            AssertMsg(spRecord != nullptr, "We have a gap in the sequence of propertyIds. Not sure how that happens.");

            const Js::PropertyRecord* newPropertyRecord = NSSnapType::InflatePropertyRecord(spRecord, this->m_threadContext);

            if(!this->m_propertyRecordPinSet->ContainsKey(const_cast<Js::PropertyRecord*>(newPropertyRecord)))
            {
                this->m_propertyRecordPinSet->AddNew(const_cast<Js::PropertyRecord*>(newPropertyRecord));
            }
        }
    }

    void EventLog::StartTimeTravelOnScript(Js::ScriptContext* ctx, const HostScriptContextCallbackFunctor& callbackFunctor)
    {
        AssertMsg(this->m_ttdContext == nullptr, "Should only add 1 time!");

        ctx->TTDMode = this->m_currentMode;
        ctx->TTDHostCallbackFunctor = callbackFunctor;

        this->m_ttdContext = ctx;

        ctx->InitializeRecordingActionsAsNeeded_TTD();
    }

    void EventLog::StopTimeTravelOnScript(Js::ScriptContext* ctx)
    {
        AssertMsg(this->m_ttdContext == ctx, "Should be enabled before we disable!");

        ctx->TTDMode = TTDMode::Detached;
        this->m_ttdContext = nullptr;
    }

    void EventLog::SetGlobalMode(TTDMode m)
    {
        AssertMsg(m == TTDMode::Pending || m == TTDMode::Detached || m == TTDMode::RecordEnabled || m == TTDMode::DebuggingEnabled, "These are the only valid global modes");

        this->m_modeStack.SetItem(0, m);
        this->UpdateComputedMode();
    }

    void EventLog::PushMode(TTDMode m)
    {
        AssertMsg(m == TTDMode::ExcludedExecution, "These are the only valid mode modifiers to push");

        this->m_modeStack.Add(m);
        this->UpdateComputedMode();
    }

    void EventLog::PopMode(TTDMode m)
    {
        AssertMsg(m == TTDMode::ExcludedExecution, "These are the only valid mode modifiers to push");
        AssertMsg(this->m_modeStack.Last() == m, "Push/Pop is not matched so something went wrong.");

        this->m_modeStack.RemoveAtEnd();
        this->UpdateComputedMode();
    }

    void EventLog::SetIntoDebuggingMode()
    {
        this->m_modeStack.SetItem(0, TTDMode::DebuggingEnabled);
        this->UpdateComputedMode();

        this->m_ttdContext->InitializeDebuggingActionsAsNeeded_TTD();
    }

    void EventLog::AddPropertyRecord(const Js::PropertyRecord* record)
    {
        this->m_propertyRecordPinSet->AddNew(const_cast<Js::PropertyRecord*>(record));
    }

    const NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo* EventLog::AddScriptLoad(Js::FunctionBody* fb, Js::ModuleID moduleId, DWORD_PTR documentID, LPCWSTR source, uint32 sourceLen, LoadScriptFlag loadFlag)
    {
        NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo* fbInfo = this->m_loadedTopLevelScripts.NextOpenEntry();
        uint64 fCount = (this->m_loadedTopLevelScripts.Count() + this->m_newFunctionTopLevelScripts.Count() + this->m_evalTopLevelScripts.Count());

        NSSnapValues::ExtractTopLevelLoadedFunctionBodyInfo(fbInfo, fb, fCount, moduleId, documentID, source, sourceLen, loadFlag, this->m_miscSlabAllocator);

        return fbInfo;
    }

    const NSSnapValues::TopLevelNewFunctionBodyResolveInfo* EventLog::AddNewFunction(Js::FunctionBody* fb, Js::ModuleID moduleId, LPCWSTR source, uint32 sourceLen)
    {
        NSSnapValues::TopLevelNewFunctionBodyResolveInfo* fbInfo = this->m_newFunctionTopLevelScripts.NextOpenEntry();
        uint64 fCount = (this->m_loadedTopLevelScripts.Count() + this->m_newFunctionTopLevelScripts.Count() + this->m_evalTopLevelScripts.Count());

        NSSnapValues::ExtractTopLevelNewFunctionBodyInfo(fbInfo, fb, fCount, moduleId, source, sourceLen, this->m_miscSlabAllocator);

        return fbInfo;
    }

    const NSSnapValues::TopLevelEvalFunctionBodyResolveInfo* EventLog::AddEvalFunction(Js::FunctionBody* fb, Js::ModuleID moduleId, LPCWSTR source, uint32 sourceLen, uint32 grfscr, bool registerDocument, BOOL isIndirect, BOOL strictMode)
    {
        NSSnapValues::TopLevelEvalFunctionBodyResolveInfo* fbInfo = this->m_evalTopLevelScripts.NextOpenEntry();
        uint64 fCount = (this->m_loadedTopLevelScripts.Count() + this->m_newFunctionTopLevelScripts.Count() + this->m_evalTopLevelScripts.Count());

        NSSnapValues::ExtractTopLevelEvalFunctionBodyInfo(fbInfo, fb, fCount, moduleId, source, sourceLen, grfscr, registerDocument, isIndirect, strictMode, this->m_miscSlabAllocator);

        return fbInfo;
    }

    void EventLog::RecordTopLevelCodeAction(uint64 bodyCtrId)
    {
        NSLogEvents::CodeLoadEventLogEntry* clEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::CodeLoadEventLogEntry, NSLogEvents::EventKind::TopLevelCodeTag>();
        clEvent->BodyCounterId = bodyCtrId;
    }

    uint64 EventLog::ReplayTopLevelCodeAction()
    {
        const NSLogEvents::CodeLoadEventLogEntry* clEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::CodeLoadEventLogEntry, NSLogEvents::EventKind::TopLevelCodeTag>();

        return clEvent->BodyCounterId;
    }

    void EventLog::RecordTelemetryLogEvent(Js::JavascriptString* infoStringJs, bool doPrint)
    {
        NSLogEvents::TelemetryEventLogEntry* tEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::TelemetryEventLogEntry, NSLogEvents::EventKind::TelemetryLogTag>();
        this->m_eventSlabAllocator.CopyStringIntoWLength(infoStringJs->GetSz(), infoStringJs->GetLength(), tEvent->InfoString);
        tEvent->DoPrint = doPrint;

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.ForceFlush();
#endif
    }

    void EventLog::ReplayTelemetryLogEvent(Js::JavascriptString* infoStringJs)
    {
#if !ENABLE_TTD_INTERNAL_DIAGNOSTICS
        this->AdvanceTimeAndPositionForReplay(); //just eat the telemetry event
#else
        const NSLogEvents::TelemetryEventLogEntry* tEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::TelemetryEventLogEntry, NSLogEvents::EventKind::TelemetryLogTag>();

        uint32 infoStrLength = (uint32)infoStringJs->GetLength();
        LPCWSTR infoStr = infoStringJs->GetSz();

        if(tEvent->InfoString.Length != infoStrLength)
        {
            AssertMsg(false, "Telemetry messages differ??");
        }
        else
        {
            for(uint32 i = 0; i < infoStrLength; ++i)
            {
                AssertMsg(tEvent->InfoString.Contents[i] == infoStr[i], "Telemetry messages differ??");
            }
        }
#endif

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.ForceFlush();
#endif
    }

    void EventLog::RecordDateTimeEvent(double time)
    {
        NSLogEvents::DoubleEventLogEntry* dEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::DoubleEventLogEntry, NSLogEvents::EventKind::DoubleTag>();
        dEvent->DoubleValue = time;
    }

    void EventLog::RecordDateStringEvent(Js::JavascriptString* stringValue)
    {
        NSLogEvents::StringValueEventLogEntry* sEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::StringValueEventLogEntry, NSLogEvents::EventKind::StringTag>();
        this->m_eventSlabAllocator.CopyStringIntoWLength(stringValue->GetSz(), stringValue->GetLength(), sEvent->StringValue);
    }

    void EventLog::ReplayDateTimeEvent(double* result)
    {
        const NSLogEvents::DoubleEventLogEntry* dEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::DoubleEventLogEntry, NSLogEvents::EventKind::DoubleTag>();
        *result = dEvent->DoubleValue;
    }

    void EventLog::ReplayDateStringEvent(Js::ScriptContext* ctx, Js::JavascriptString** result)
    {
        const NSLogEvents::StringValueEventLogEntry* sEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::StringValueEventLogEntry, NSLogEvents::EventKind::StringTag>();

        const TTString& str = sEvent->StringValue;
        *result = Js::JavascriptString::NewCopyBuffer(str.Contents, str.Length, ctx);
    }

    void EventLog::RecordExternalEntropyRandomEvent(uint64 seed0, uint64 seed1)
    {
        NSLogEvents::RandomSeedEventLogEntry* rsEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::RandomSeedEventLogEntry, NSLogEvents::EventKind::RandomSeedTag>();
        rsEvent->Seed0 = seed0;
        rsEvent->Seed1 = seed1;
    }

    void EventLog::ReplayExternalEntropyRandomEvent(uint64* seed0, uint64* seed1)
    {
        const NSLogEvents::RandomSeedEventLogEntry* rsEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::RandomSeedEventLogEntry, NSLogEvents::EventKind::RandomSeedTag>();
        *seed0 = rsEvent->Seed0;
        *seed1 = rsEvent->Seed1;
    }

    void EventLog::RecordPropertyEnumEvent(BOOL returnCode, Js::PropertyId pid, Js::PropertyAttributes attributes, Js::JavascriptString* propertyName)
    {
        NSLogEvents::PropertyEnumStepEventLogEntry* peEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::PropertyEnumStepEventLogEntry, NSLogEvents::EventKind::PropertyEnumTag>();
        peEvent->ReturnCode = returnCode;
        peEvent->Pid = pid;
        peEvent->Attributes = attributes;

        InitializeAsNullPtrTTString(peEvent->PropertyString);
#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        if(returnCode)
        {
            this->m_eventSlabAllocator.CopyStringIntoWLength(propertyName->GetSz(), propertyName->GetLength(), peEvent->PropertyString);
        }
#else
        if(returnCode && pid == Js::Constants::NoProperty)
        {
            this->m_eventSlabAllocator.CopyStringIntoWLength(propertyName->GetSz(), propertyName->GetLength(), peEvent->PropertyString);
        }
#endif
    }

    void EventLog::ReplayPropertyEnumEvent(BOOL* returnCode, int32* newIndex, const Js::DynamicObject* obj, Js::PropertyId* pid, Js::PropertyAttributes* attributes, Js::JavascriptString** propertyName)
    {
        const NSLogEvents::PropertyEnumStepEventLogEntry* peEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::PropertyEnumStepEventLogEntry, NSLogEvents::EventKind::PropertyEnumTag>();

        *returnCode = peEvent->ReturnCode;
        *pid = peEvent->Pid;
        *attributes = peEvent->Attributes;

        if(*returnCode)
        {
            AssertMsg(*pid != Js::Constants::NoProperty, "This is so weird we need to figure out what this means.");
            Js::PropertyString* propertyString = obj->GetScriptContext()->GetPropertyString(*pid);
            *propertyName = propertyString;

            const Js::PropertyRecord* pRecord = obj->GetScriptContext()->GetPropertyName(*pid);
            *newIndex = obj->GetDynamicType()->GetTypeHandler()->GetPropertyIndex(pRecord);
        }
        else
        {
            *propertyName = nullptr;

            *newIndex = obj->GetDynamicType()->GetTypeHandler()->GetPropertyCount();
        }
    }

    void EventLog::RecordSymbolCreationEvent(Js::PropertyId pid)
    {
        NSLogEvents::SymbolCreationEventLogEntry* scEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::SymbolCreationEventLogEntry, NSLogEvents::EventKind::SymbolCreationTag>();
        scEvent->Pid = pid;
    }

    void EventLog::ReplaySymbolCreationEvent(Js::PropertyId* pid)
    {
        const NSLogEvents::SymbolCreationEventLogEntry* scEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::SymbolCreationEventLogEntry, NSLogEvents::EventKind::SymbolCreationTag>();
        *pid = scEvent->Pid;
    }

    NSLogEvents::EventLogEntry* EventLog::RecordExternalCallEvent(Js::JavascriptFunction* func, int32 rootDepth, uint32 argc, Js::Var* argv)
    {
        NSLogEvents::EventLogEntry* evt = nullptr;
        this->RecordGetInitializedEvent_HelperWithMainEvent<NSLogEvents::ExternalCallEventLogEntry, NSLogEvents::EventKind::ExternalCallTag>(&evt);

        NSLogEvents::ExternalCallEventLogEntry_ProcessArgs(evt, rootDepth, func, argc, argv, this->m_eventSlabAllocator);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        NSLogEvents::ExternalCallEventLogEntry_ProcessDiagInfoPre(evt, func, this->m_eventSlabAllocator);
#endif

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteCall(func, true, argc, argv, this->GetLastEventTime());
#endif

        return evt;
    }

    void EventLog::RecordExternalCallEvent_Complete(NSLogEvents::EventLogEntry* evt, Js::JavascriptFunction* func, bool normalReturn, bool checkException, Js::Var result)
    {
        Js::ScriptContext* ctx = func->GetScriptContext();

        //
        //TODO: we will want to be a bit more detailed on this later
        //
        bool hasScriptException = false;
        bool hasTerminalException = false;
        if(normalReturn)
        {
            hasScriptException = checkException ? ctx->HasRecordedException() : false;
            hasTerminalException = false;
        }
        else
        {
            hasScriptException = true;
            hasTerminalException = false;
        }

        NSLogEvents::ExternalCallEventLogEntry_ProcessReturn(evt, result, hasScriptException, hasTerminalException, this->GetLastEventTime());

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteReturn(func, result, this->GetLastEventTime());
#endif
    }

    void EventLog::ReplayExternalCallEvent(Js::JavascriptFunction* function, uint32 argc, Js::Var* argv, Js::Var* result)
    {
        const NSLogEvents::ExternalCallEventLogEntry* ecEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::ExternalCallEventLogEntry, NSLogEvents::EventKind::ExternalCallTag>();

        Js::ScriptContext* ctx = function->GetScriptContext();

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteCall(function, true, argc, argv, this->GetLastEventTime());
#endif

        //make sure we log all of the passed arguments in the replay host
        AssertMsg(argc + 1 == ecEvent->ArgCount, "Mismatch in args!!!");

        TTDVar recordedFunction = ecEvent->ArgArray[0];
        NSLogEvents::PassVarToHostInReplay(ctx, recordedFunction, function);

        for(uint32 i = 0; i < argc; ++i)
        {
            Js::Var replayVar = argv[i];
            TTDVar recordedVar = ecEvent->ArgArray[i + 1];
            NSLogEvents::PassVarToHostInReplay(ctx, recordedVar, replayVar);
        }

        //replay anything that happens in the external call
        if(ecEvent->AdditionalInfo->LastNestedEventTime >= this->m_eventTimeCtr)
        {
            if(!ctx->GetThreadContext()->IsScriptActive())
            {
                this->ReplayActionLoopRange(ecEvent->AdditionalInfo->LastNestedEventTime);
            }
            else
            {
                BEGIN_LEAVE_SCRIPT_WITH_EXCEPTION(ctx)
                {
                    this->ReplayActionLoopRange(ecEvent->AdditionalInfo->LastNestedEventTime);
                }
                END_LEAVE_SCRIPT_WITH_EXCEPTION(ctx);
            }
        }

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        AssertMsg(!this->m_currentReplayEventIterator.IsValid() || this->m_currentReplayEventIterator.Current()->EventTimeStamp == this->m_eventTimeCtr, "Out of Sync!!!");
#endif

        *result = NSLogEvents::InflateVarInReplay(ctx, ecEvent->ReturnValue);

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteReturn(function, *result, this->GetLastEventTime());
#endif
    }

    NSLogEvents::EventLogEntry* EventLog::RecordEnqueueTaskEvent(Js::Var taskVar)
    {
        NSLogEvents::EventLogEntry* evt = nullptr;
        NSLogEvents::ExternalCbRegisterCallEventLogEntry* ecEvent = this->RecordGetInitializedEvent_HelperWithMainEvent<NSLogEvents::ExternalCbRegisterCallEventLogEntry, NSLogEvents::EventKind::ExternalCbRegisterCall>(&evt);

        ecEvent->CallbackFunction = static_cast<TTDVar>(taskVar);

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteLiteralMsg("Enqueue Task: ");
        this->m_diagnosticLogger.WriteVar(taskVar);
#endif

        return evt;
    }

    void EventLog::RecordEnqueueTaskEvent_Complete(NSLogEvents::EventLogEntry* evt)
    {
        NSLogEvents::ExternalCbRegisterCallEventLogEntry* ecEvent = NSLogEvents::GetInlineEventDataAs<NSLogEvents::ExternalCbRegisterCallEventLogEntry, NSLogEvents::EventKind::ExternalCbRegisterCall>(evt);

        ecEvent->LastNestedEventTime = this->GetLastEventTime();
    }

    void EventLog::ReplayEnqueueTaskEvent(Js::ScriptContext* ctx, Js::Var taskVar)
    {
        const NSLogEvents::ExternalCbRegisterCallEventLogEntry* ecEvent = this->ReplayGetReplayEvent_Helper<NSLogEvents::ExternalCbRegisterCallEventLogEntry, NSLogEvents::EventKind::ExternalCbRegisterCall>();

        NSLogEvents::PassVarToHostInReplay(ctx, ecEvent->CallbackFunction, taskVar);

        //replay anything that happens when we are out of the call
        if(ecEvent->LastNestedEventTime >= this->m_eventTimeCtr)
        {
            if(!ctx->GetThreadContext()->IsScriptActive())
            {
                this->ReplayActionLoopRange(ecEvent->LastNestedEventTime);
            }
            else
            {
                BEGIN_LEAVE_SCRIPT_WITH_EXCEPTION(ctx)
                {
                    this->ReplayActionLoopRange(ecEvent->LastNestedEventTime);
                }
                END_LEAVE_SCRIPT_WITH_EXCEPTION(ctx);
            }
        }

        //May have exited inside the external call without anything else
        if(!this->m_currentReplayEventIterator.IsValid())
        {
            this->AbortReplayReturnToHost();
        }

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        AssertMsg(!this->m_currentReplayEventIterator.IsValid() || this->m_currentReplayEventIterator.Current()->EventTimeStamp == this->m_eventTimeCtr, "Out of Sync!!!");
#endif
    }

    void EventLog::PushCallEvent(Js::JavascriptFunction* function, uint32 argc, Js::Var* argv, bool isInFinally)
    {
#if ENABLE_TTD_DEBUGGING
        //Clear any previous last return frame info
        this->ClearReturnFrame();
#endif

        this->m_runningFunctionTimeCtr++;

        SingleCallCounter cfinfo;
        cfinfo.Function = function->GetFunctionBody();

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        cfinfo.Name = cfinfo.Function->GetExternalDisplayName();
#endif

        cfinfo.EventTime = this->m_eventTimeCtr; //don't need to advance just note what the event time was when this is called
        cfinfo.FunctionTime = this->m_runningFunctionTimeCtr;
        cfinfo.LoopTime = 0;

#if ENABLE_TTD_STACK_STMTS
        cfinfo.CurrentStatementIndex = -1;
        cfinfo.CurrentStatementLoopTime = 0;

        cfinfo.LastStatementIndex = -1;
        cfinfo.LastStatementLoopTime = 0;

        cfinfo.CurrentStatementBytecodeMin = UINT32_MAX;
        cfinfo.CurrentStatementBytecodeMax = UINT32_MAX;
#endif

        this->m_callStack.Add(cfinfo);

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteCall(function, false, argc, argv, this->m_eventTimeCtr);
#endif
    }

    void EventLog::PopCallEvent(Js::JavascriptFunction* function, Js::Var result)
    {
#if ENABLE_TTD_DEBUGGING
        if(!this->HasImmediateExceptionFrame())
        {
            this->SetReturnAndExceptionFramesFromCurrent(true, false);
        }
#endif

        this->m_runningFunctionTimeCtr++;
        this->m_callStack.RemoveAtEnd();

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteReturn(function, result, this->m_eventTimeCtr);
#endif
    }

    void EventLog::PopCallEventException(Js::JavascriptFunction* function, bool isFirstException)
    {
#if ENABLE_TTD_DEBUGGING
        if(isFirstException)
        {
            this->SetReturnAndExceptionFramesFromCurrent(false, true);
        }
#endif

        this->m_runningFunctionTimeCtr++;
        this->m_callStack.RemoveAtEnd();

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteReturnException(function, this->m_eventTimeCtr);
#endif
    }

#if ENABLE_TTD_DEBUGGING
    bool EventLog::HasImmediateReturnFrame() const
    {
        return this->m_isReturnFrame;
    }

    bool EventLog::HasImmediateExceptionFrame() const
    {
        return this->m_isExceptionFrame;
    }

    const SingleCallCounter& EventLog::GetImmediateReturnFrame() const
    {
        AssertMsg(this->m_isReturnFrame, "This data is invalid if we haven't recorded a return!!!");

        return this->m_lastFrame;
    }

    const SingleCallCounter& EventLog::GetImmediateExceptionFrame() const
    {
        AssertMsg(this->m_isExceptionFrame, "This data is invalid if we haven't recorded an exception!!!");

        return this->m_lastFrame;
    }

    void EventLog::ClearReturnFrame()
    {
        this->m_isReturnFrame = false;
    }

    void EventLog::ClearExceptionFrame()
    {
        this->m_isExceptionFrame = false;
    }

    void EventLog::SetReturnAndExceptionFramesFromCurrent(bool setReturn, bool setException)
    {
        AssertMsg(this->m_callStack.Count() != 0, "We must have pushed something in order to have an exception or return!!!");
        AssertMsg((setReturn | setException) & (!setReturn | !setException), "We can only have a return or exception -- exactly one not both!!!");

        this->m_isReturnFrame = setReturn;
        this->m_isExceptionFrame = setException;

        this->m_lastFrame = this->m_callStack.Last();
    }

    bool EventLog::HasPendingTTDBP() const
    {
        return this->m_pendingTTDBP.HasValue();
    }

    int64 EventLog::GetPendingTTDBPTargetEventTime() const
    {
        return this->m_pendingTTDBP.GetRootEventTime();
    }

    void EventLog::GetPendingTTDBPInfo(TTDebuggerSourceLocation& BPLocation) const
    {
        BPLocation.SetLocation(this->m_pendingTTDBP);
    }

    void EventLog::ClearPendingTTDBPInfo()
    {
        this->m_pendingTTDBP.Clear();
    }

    void EventLog::SetPendingTTDBPInfo(const TTDebuggerSourceLocation& BPLocation)
    {
        this->m_pendingTTDBP.SetLocation(BPLocation);
    }

    bool EventLog::HasActiveBP() const
    {
        return this->m_activeBPId != -1;
    }

    UINT EventLog::GetActiveBPId() const
    {
        AssertMsg(this->HasActiveBP(), "Should check this first!!!");

        return (UINT)this->m_activeBPId;
    }

    void EventLog::ClearActiveBP()
    {
        this->m_activeBPId = -1;
        this->m_activeTTDBP.Clear();
    }

    void EventLog::SetActiveBP(UINT bpId, const TTDebuggerSourceLocation& bpLocation)
    {
        this->m_activeBPId = bpId;
        this->m_activeTTDBP.SetLocation(bpLocation);
    }

    bool EventLog::ProcessBPInfoPreBreak(Js::FunctionBody* fb)
    {
        if(!fb->GetScriptContext()->ShouldPerformDebugAction())
        {
            return true;
        }

        if(!this->HasActiveBP())
        {
            return true;
        }

        const SingleCallCounter& cfinfo = this->GetTopCallCounter();
        ULONG srcLine = 0;
        LONG srcColumn = -1;
        uint32 startOffset = cfinfo.Function->GetStatementStartOffset(cfinfo.CurrentStatementIndex);
        cfinfo.Function->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

        bool locationOk = ((uint32)srcLine == this->m_activeTTDBP.GetLine()) & ((uint32)srcColumn == this->m_activeTTDBP.GetColumn());
        bool ftimeOk = (this->m_activeTTDBP.GetFunctionTime() == -1) | ((uint64)this->m_activeTTDBP.GetFunctionTime() == cfinfo.FunctionTime);
        bool ltimeOk = (this->m_activeTTDBP.GetLoopTime() == -1) | ((uint64)this->m_activeTTDBP.GetLoopTime() == cfinfo.CurrentStatementLoopTime);

        return locationOk & ftimeOk & ltimeOk;
    }

    void EventLog::ProcessBPInfoPostBreak(Js::FunctionBody* fb)
    {
        if(!fb->GetScriptContext()->ShouldPerformDebugAction())
        {
            return;
        }

        if(this->HasActiveBP())
        {
            Js::DebugDocument* debugDocument = fb->GetUtf8SourceInfo()->GetDebugDocument();
            Js::StatementLocation statement;
            if(debugDocument->FindBPStatementLocation(this->GetActiveBPId(), &statement))
            {
                debugDocument->SetBreakPoint(statement, BREAKPOINT_DELETED);
            }

            this->ClearActiveBP();
        }

        if(this->HasPendingTTDBP())
        {
            throw TTD::TTDebuggerAbortException::CreateTopLevelAbortRequest(this->GetPendingTTDBPTargetEventTime(), _u("Reverse operation requested."));
        }
    }
#endif

    void EventLog::UpdateLoopCountInfo()
    {
        SingleCallCounter& cfinfo = this->m_callStack.Last();
        cfinfo.LoopTime++;
    }

#if ENABLE_TTD_STACK_STMTS
    void EventLog::UpdateCurrentStatementInfo(uint bytecodeOffset)
    {
        SingleCallCounter& cfinfo = this->GetTopCallCounter();
        if((cfinfo.CurrentStatementBytecodeMin <= bytecodeOffset) & (bytecodeOffset <= cfinfo.CurrentStatementBytecodeMax))
        {
            return;
        }
        else
        {
            Js::FunctionBody* fb = cfinfo.Function;

            int32 cIndex = fb->GetEnclosingStatementIndexFromByteCode(bytecodeOffset, true);
            AssertMsg(cIndex != -1, "Should always have a mapping.");

            //we moved to a new statement
            Js::FunctionBody::StatementMap* pstmt = fb->GetStatementMaps()->Item(cIndex);
            bool newstmt = (cIndex != cfinfo.CurrentStatementIndex && pstmt->byteCodeSpan.begin <= (int)bytecodeOffset && (int)bytecodeOffset <= pstmt->byteCodeSpan.end);
            if(newstmt)
            {
                cfinfo.LastStatementIndex = cfinfo.CurrentStatementIndex;
                cfinfo.LastStatementLoopTime = cfinfo.CurrentStatementLoopTime;

                cfinfo.CurrentStatementIndex = cIndex;
                cfinfo.CurrentStatementLoopTime = cfinfo.LoopTime;

                cfinfo.CurrentStatementBytecodeMin = (uint32)pstmt->byteCodeSpan.begin;
                cfinfo.CurrentStatementBytecodeMax = (uint32)pstmt->byteCodeSpan.end;

#if ENABLE_FULL_BC_TRACE
                ULONG srcLine = 0;
                LONG srcColumn = -1;
                uint32 startOffset = cfinfo.Function->GetFunctionBody()->GetStatementStartOffset(cfinfo.CurrentStatementIndex);
                cfinfo.Function->GetFunctionBody()->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

                this->m_diagnosticLogger.WriteStmtIndex((uint32)srcLine, (uint32)srcColumn);
#endif
            }
        }
    }

    void EventLog::GetTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const
    {
        const SingleCallCounter& cfinfo = this->GetTopCallCounter();

        ULONG srcLine = 0;
        LONG srcColumn = -1;
        uint32 startOffset = cfinfo.Function->GetStatementStartOffset(cfinfo.CurrentStatementIndex);
        cfinfo.Function->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

        sourceLocation.SetLocation(this->m_topLevelCallbackEventTime, cfinfo.FunctionTime, cfinfo.LoopTime, cfinfo.Function, srcLine, srcColumn);
    }
#endif

#if ENABLE_OBJECT_SOURCE_TRACKING
    void EventLog::GetTimeAndPositionForDiagnosticObjectTracking(DiagnosticOrigin& originInfo) const
    {
        const SingleCallCounter& cfinfo = this->GetTopCallCounter();

        ULONG srcLine = 0;
        LONG srcColumn = -1;
        uint32 startOffset = cfinfo.Function->GetStatementStartOffset(cfinfo.CurrentStatementIndex);
        cfinfo.Function->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

        SetDiagnosticOriginInformation(originInfo, srcLine, cfinfo.EventTime, cfinfo.FunctionTime, cfinfo.LoopTime);
    }
#endif 

#if ENABLE_TTD_DEBUGGING
    bool EventLog::GetPreviousTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const
    {
        const SingleCallCounter& cfinfo = this->GetTopCallCounter();

        //check if we are at the first statement in the callback event
        if(this->m_callStack.Count() == 1 && cfinfo.LastStatementIndex == -1)
        {
            //Set the position info to the current statement and return true
            this->GetTimeAndPositionForDebugger(sourceLocation);

            return true;
        }

        //if we are at the first statement in the function then we want the parents current
        Js::FunctionBody* fbody = nullptr;
        int32 statementIndex = -1;
        uint64 ftime = 0;
        uint64 ltime = 0;
        if(cfinfo.LastStatementIndex == -1)
        {
            const SingleCallCounter& cfinfoCaller = this->GetTopCallCallerCounter();
            ftime = cfinfoCaller.FunctionTime;
            ltime = cfinfoCaller.CurrentStatementLoopTime;

            fbody = cfinfoCaller.Function;
            statementIndex = cfinfoCaller.CurrentStatementIndex;
        }
        else
        {
            ftime = cfinfo.FunctionTime;
            ltime = cfinfo.LastStatementLoopTime;

            fbody = cfinfo.Function;
            statementIndex = cfinfo.LastStatementIndex;
        }

        ULONG srcLine = 0;
        LONG srcColumn = -1;
        uint32 startOffset = fbody->GetStatementStartOffset(statementIndex);
        fbody->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

        sourceLocation.SetLocation(this->m_topLevelCallbackEventTime, ftime, ltime, fbody, srcLine, srcColumn);

        return false;
    }

    bool EventLog::GetExceptionTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const
    {
        if(!this->m_isExceptionFrame)
        {
            sourceLocation.Clear();
            return false;
        }
        else
        {
            ULONG srcLine = 0;
            LONG srcColumn = -1;
            uint32 startOffset = this->m_lastFrame.Function->GetStatementStartOffset(this->m_lastFrame.CurrentStatementIndex);
            this->m_lastFrame.Function->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

            sourceLocation.SetLocation(this->m_topLevelCallbackEventTime, this->m_lastFrame.FunctionTime, this->m_lastFrame.CurrentStatementLoopTime, this->m_lastFrame.Function, srcLine, srcColumn);
            return true;
        }
    }

    bool EventLog::GetImmediateReturnTimeAndPositionForDebugger(TTDebuggerSourceLocation& sourceLocation) const
    {
        if(!this->m_isReturnFrame)
        {
            sourceLocation.Clear();
            return false;
        }
        else
        {
            ULONG srcLine = 0;
            LONG srcColumn = -1;
            uint32 startOffset = this->m_lastFrame.Function->GetStatementStartOffset(this->m_lastFrame.CurrentStatementIndex);
            this->m_lastFrame.Function->GetSourceLineFromStartOffset_TTD(startOffset, &srcLine, &srcColumn);

            sourceLocation.SetLocation(this->m_topLevelCallbackEventTime, this->m_lastFrame.FunctionTime, this->m_lastFrame.CurrentStatementLoopTime, this->m_lastFrame.Function, srcLine, srcColumn);
            return true;
        }
    }

    int64 EventLog::GetCurrentHostCallbackId() const
    {
        return this->m_hostCallbackId;
    }

    int64 EventLog::GetCurrentTopLevelEventTime() const
    {
        return this->m_topLevelCallbackEventTime;
    }

    const NSLogEvents::JsRTCallbackAction* EventLog::GetEventForHostCallbackId(bool wantRegisterOp, int64 hostIdOfInterest) const
    {
        if(hostIdOfInterest == -1)
        {
            return nullptr;
        }

        for(auto iter = this->m_currentReplayEventIterator; iter.IsValid(); iter.MovePrevious())
        {
            if(iter.Current()->EventKind == NSLogEvents::EventKind::CallbackOpActionTag)
            {
                const NSLogEvents::JsRTCallbackAction* callbackAction = NSLogEvents::GetInlineEventDataAs<NSLogEvents::JsRTCallbackAction, NSLogEvents::EventKind::CallbackOpActionTag>(iter.Current());
                if(callbackAction->NewCallbackId == hostIdOfInterest && callbackAction->IsCreate == wantRegisterOp)
                {
                    return callbackAction;
                }
            }
        }

        return nullptr;
    }

    int64 EventLog::GetKthEventTime(uint32 k) const
    {
        uint32 topLevelCount = 0;
        for(auto iter = this->m_eventList.GetIteratorAtFirst(); iter.IsValid(); iter.MoveNext())
        {
            if(NSLogEvents::IsJsRTActionRootCall(iter.Current()))
            {
                topLevelCount++;

                if(topLevelCount == k)
                {
                    return NSLogEvents::GetTimeFromRootCallOrSnapshot(iter.Current());
                }
            }
        }

        AssertMsg(false, "Bad event index!!!");
        return -1;
    }
#endif

    void EventLog::ResetCallStackForTopLevelCall(int64 topLevelCallbackEventTime, int64 hostCallbackId)
    {
        AssertMsg(this->m_callStack.Count() == 0, "We should be at the top-level entry!!!");

        this->m_runningFunctionTimeCtr = 0;
        this->m_topLevelCallbackEventTime = topLevelCallbackEventTime;
        this->m_hostCallbackId = hostCallbackId;

#if ENABLE_TTD_DEBUGGING
        this->ClearReturnFrame();
        this->ClearExceptionFrame();
#endif
    }

    bool EventLog::IsTimeForSnapshot() const
    {
        return (this->m_elapsedExecutionTimeSinceSnapshot > this->m_snapInterval);
    }

    void EventLog::PruneLogLength()
    {
        //
        //TODO: add code to see if we have more snapshots than the specified limit and if so unload them
        //
    }

    void EventLog::IncrementElapsedSnapshotTime(double addtlTime)
    {
        this->m_elapsedExecutionTimeSinceSnapshot += addtlTime;
    }

    void EventLog::DoSnapshotExtract()
    {
        AssertMsg(this->m_ttdContext != nullptr, "We aren't actually tracking anything!!!");

        ///////////////////////////
        //Create the event object and add it to the log
        NSLogEvents::SnapshotEventLogEntry* snapEvent = this->RecordGetInitializedEvent_Helper<NSLogEvents::SnapshotEventLogEntry, NSLogEvents::EventKind::SnapshotTag>();
        snapEvent->RestoreTimestamp = this->GetLastEventTime();
        snapEvent->Snap = this->DoSnapshotExtract_Helper();

        this->m_elapsedExecutionTimeSinceSnapshot = 0.0;

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteLiteralMsg("---SNAPSHOT EVENT---\n");
#endif
    }

    void EventLog::DoRtrSnapIfNeeded()
    {
        AssertMsg(this->m_ttdContext != nullptr, "We aren't actually tracking anything!!!");
        AssertMsg(this->m_currentReplayEventIterator.IsValid() && NSLogEvents::IsJsRTActionRootCall(this->m_currentReplayEventIterator.Current()), "Something in wrong with the event position.");

        NSLogEvents::JsRTCallFunctionAction* rootCall = NSLogEvents::GetInlineEventDataAs<NSLogEvents::JsRTCallFunctionAction, NSLogEvents::EventKind::CallExistingFunctionActionTag>(this->m_currentReplayEventIterator.Current());
        if(rootCall->AdditionalInfo->RtRSnap == nullptr)
        {
            rootCall->AdditionalInfo->RtRSnap = this->DoSnapshotExtract_Helper();
        }
    }

    int64 EventLog::FindSnapTimeForEventTime(int64 targetTime, bool* newCtxsNeeded)
    {
        *newCtxsNeeded = false;
        int64 snapTime = -1;

        for(auto iter = this->m_eventList.GetIteratorAtLast(); iter.IsValid(); iter.MovePrevious())
        {
            bool isSnap = false;
            bool isRoot = false;
            bool hasRtrSnap = false;
            int64 time = NSLogEvents::AccessTimeInRootCallOrSnapshot(iter.Current(), isSnap, isRoot, hasRtrSnap);

            bool validSnap = isSnap | (isRoot & hasRtrSnap);
            if(validSnap && time <= targetTime)
            {
                snapTime = time;
                break;
            }
        }

        //if this->m_lastInflateMap then this is the first time we have inflated (otherwise we always nullify and recreate as a pair)
        if(this->m_lastInflateMap != nullptr)
        {
            *newCtxsNeeded = (snapTime != this->m_lastInflateSnapshotTime);
        }

        return snapTime;
    }

    void EventLog::UpdateInflateMapForFreshScriptContexts()
    {
        this->m_ttdContext = nullptr;

        if(this->m_lastInflateMap != nullptr)
        {
            HeapDelete(this->m_lastInflateMap);
            this->m_lastInflateMap = nullptr;
        }
    }

    void EventLog::DoSnapshotInflate(int64 etime)
    {
        //collect anything that is dead
        this->m_threadContext->GetRecycler()->CollectNow<CollectNowForceInThread>();

        const SnapShot* snap = nullptr;
        int64 restoreEventTime = -1;

        for(auto iter = this->m_eventList.GetIteratorAtLast(); iter.IsValid(); iter.MovePrevious())
        {
            NSLogEvents::EventLogEntry* evt = iter.Current();
            if(evt->EventKind == NSLogEvents::EventKind::SnapshotTag)
            {
                NSLogEvents::SnapshotEventLogEntry* snapEvent = NSLogEvents::GetInlineEventDataAs<NSLogEvents::SnapshotEventLogEntry, NSLogEvents::EventKind::SnapshotTag>(evt);
                if(snapEvent->RestoreTimestamp == etime)
                {
                    NSLogEvents::SnapshotEventLogEntry_EnsureSnapshotDeserialized(evt, this->m_logInfoRootDir.Contents, this->m_threadContext);

                    restoreEventTime = snapEvent->RestoreTimestamp;
                    snap = snapEvent->Snap;
                    break;
                }
            }

            if(NSLogEvents::IsJsRTActionRootCall(evt))
            {
                const NSLogEvents::JsRTCallFunctionAction* rootEntry = NSLogEvents::GetInlineEventDataAs<NSLogEvents::JsRTCallFunctionAction, NSLogEvents::EventKind::CallExistingFunctionActionTag>(evt);

                if(rootEntry->AdditionalInfo->CallEventTime == etime)
                {
                    restoreEventTime = rootEntry->AdditionalInfo->CallEventTime;
                    snap = rootEntry->AdditionalInfo->RtRSnap;
                    break;
                }
            }
        }
        AssertMsg(snap != nullptr, "Log should start with a snapshot!!!");

        TTDIdentifierDictionary<uint64, NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo*> topLevelLoadScriptMap;
        topLevelLoadScriptMap.Initialize(this->m_loadedTopLevelScripts.Count());
        for(auto iter = this->m_loadedTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            topLevelLoadScriptMap.AddItem(iter.Current()->TopLevelBase.TopLevelBodyCtr, iter.Current());
        }

        TTDIdentifierDictionary<uint64, NSSnapValues::TopLevelNewFunctionBodyResolveInfo*> topLevelNewScriptMap;
        topLevelNewScriptMap.Initialize(this->m_newFunctionTopLevelScripts.Count());
        for(auto iter = this->m_newFunctionTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            topLevelNewScriptMap.AddItem(iter.Current()->TopLevelBase.TopLevelBodyCtr, iter.Current());
        }

        TTDIdentifierDictionary<uint64, NSSnapValues::TopLevelEvalFunctionBodyResolveInfo*> topLevelEvalScriptMap;
        topLevelEvalScriptMap.Initialize(this->m_evalTopLevelScripts.Count());
        for(auto iter = this->m_evalTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            topLevelEvalScriptMap.AddItem(iter.Current()->TopLevelBase.TopLevelBodyCtr, iter.Current());
        }

        //
        //TODO: we currently assume a single context here which we load into the existing ctx
        //
        const UnorderedArrayList<NSSnapValues::SnapContext, TTD_ARRAY_LIST_SIZE_XSMALL>& snpCtxs = snap->GetContextList();
        AssertMsg(this->m_ttdContext != nullptr, "We are assuming a single context");
        const NSSnapValues::SnapContext* sCtx = snpCtxs.GetIterator().Current();

        if(this->m_lastInflateMap != nullptr)
        {
            this->m_lastInflateMap->PrepForReInflate(snap->ContextCount(), snap->HandlerCount(), snap->TypeCount(), snap->PrimitiveCount() + snap->ObjectCount(), snap->BodyCount(), snap->EnvCount(), snap->SlotArrayCount());

            NSSnapValues::InflateScriptContext(sCtx, this->m_ttdContext, this->m_lastInflateMap, topLevelLoadScriptMap, topLevelNewScriptMap, topLevelEvalScriptMap);
        }
        else
        {
            this->m_lastInflateMap = HeapNew(InflateMap);
            this->m_lastInflateMap->PrepForInitialInflate(this->m_threadContext, snap->ContextCount(), snap->HandlerCount(), snap->TypeCount(), snap->PrimitiveCount() + snap->ObjectCount(), snap->BodyCount(), snap->EnvCount(), snap->SlotArrayCount());
            this->m_lastInflateSnapshotTime = etime;

            NSSnapValues::InflateScriptContext(sCtx, this->m_ttdContext, this->m_lastInflateMap, topLevelLoadScriptMap, topLevelNewScriptMap, topLevelEvalScriptMap);

            //We don't want to have a bunch of snapshots in memory (that will get big fast) so unload all but the current one
            for(auto iter = this->m_eventList.GetIteratorAtLast(); iter.IsValid(); iter.MovePrevious())
            {
                bool isSnap = false;
                bool isRoot = false;
                bool hasRtrSnap = false;
                int64 time = NSLogEvents::AccessTimeInRootCallOrSnapshot(iter.Current(), isSnap, isRoot, hasRtrSnap);

                bool hasSnap = isSnap | (isRoot & hasRtrSnap);
                if(hasSnap && time != etime)
                {
                    if(isSnap)
                    {
                        NSLogEvents::SnapshotEventLogEntry_UnloadSnapshot(iter.Current());
                    }
                    else
                    {
                        NSLogEvents::JsRTCallFunctionAction_UnloadSnapshot(iter.Current());
                    }
                }
            }
        }

        //reset the tagged object maps before we do the inflate
        this->m_eventTimeCtr = restoreEventTime;

        snap->Inflate(this->m_lastInflateMap, sCtx);
        this->m_lastInflateMap->CleanupAfterInflate();

        if(!this->m_eventList.IsEmpty())
        {
            this->m_currentReplayEventIterator = this->m_eventList.GetIteratorAtLast();

            while(true)
            {
                bool isSnap = false;
                bool isRoot = false;
                bool hasRtrSnap = false;
                int64 time = NSLogEvents::AccessTimeInRootCallOrSnapshot(this->m_currentReplayEventIterator.Current(), isSnap, isRoot, hasRtrSnap);

                if((isSnap | isRoot) && time == this->m_eventTimeCtr)
                {
                    break;
                }

                this->m_currentReplayEventIterator.MovePrevious();
            }

            //we want to advance to the event immediately after the snapshot as well so do that
            if(this->m_currentReplayEventIterator.Current()->EventKind == NSLogEvents::EventKind::SnapshotTag)
            {
                this->m_eventTimeCtr++;
                this->m_currentReplayEventIterator.MoveNext();
            }

            //clear this out -- it shouldn't matter for most JsRT actions (alloc etc.) and should be reset by any call actions
            this->ResetCallStackForTopLevelCall(-1, -1);
        }

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.WriteLiteralMsg("---INFLATED SNAPSHOT---\n");
#endif
    }

    void EventLog::ReplaySingleEntry()
    {
        if(!this->m_currentReplayEventIterator.IsValid())
        {
            this->AbortReplayReturnToHost();
        }

        NSLogEvents::EventKind eKind = this->m_currentReplayEventIterator.Current()->EventKind;
        if(eKind == NSLogEvents::EventKind::SnapshotTag)
        {
            this->ReplaySnapshotEvent();
        }
        else if(eKind == NSLogEvents::EventKind::EventLoopYieldPointTag)
        {
            this->ReplayEventLoopYieldPointEvent();
        }
        else
        {
            AssertMsg(eKind > NSLogEvents::EventKind::JsRTActionTag, "Either this is an invalid tag to replay directly (should be driven internally) or it is not known!!!");

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
            AssertMsg(this->m_currentReplayEventIterator.Current()->EventTimeStamp == this->m_eventTimeCtr, "We are out of sync here");
#endif

            //replay a single top-level JsRT action event
            this->ReplayActionLoopRange(this->m_eventTimeCtr); 
        }
    }

    void EventLog::ReplayToTime(int64 eventTime)
    {
#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        AssertMsg(this->m_currentReplayEventIterator.IsValid() && this->m_currentReplayEventIterator.Current()->EventTimeStamp <= eventTime, "This isn't going to work.");
#endif

        while(!NSLogEvents::IsJsRTActionRootCall(this->m_currentReplayEventIterator.Current()) || NSLogEvents::GetTimeFromRootCallOrSnapshot(this->m_currentReplayEventIterator.Current()) != eventTime)
        {
            this->ReplaySingleEntry();

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
            AssertMsg(this->m_currentReplayEventIterator.IsValid() && m_currentReplayEventIterator.Current()->EventTimeStamp <= eventTime, "Something is not lined up correctly.");
#endif
        }
    }

    void EventLog::ReplayFullTrace()
    {
        while(this->m_currentReplayEventIterator.IsValid())
        {
            this->ReplaySingleEntry();
        }

        //we are at end of trace so abort to top level
        this->AbortReplayReturnToHost();
    }

    bool EventLog::IsPropertyRecordRef(void* ref) const
    {
        //This is an ugly cast but we just want to know if the pointer is in the set so it is ok here
        return this->m_propertyRecordPinSet->ContainsKey((Js::PropertyRecord*)ref);
    }

    double EventLog::GetCurrentWallTime()
    {
        return this->m_timer.Now();
    }

    int64 EventLog::GetLastEventTime() const
    {
        return this->m_eventTimeCtr - 1;
    }

#if !INT32VAR
    void EventLog::RecordJsRTCreateInteger(Js::ScriptContext* ctx, int value, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* iAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::CreateIntegerActionTag>(resultVarPtr);
        iAction->u_iVal = value;
    }
#endif

    void EventLog::RecordJsRTCreateNumber(Js::ScriptContext* ctx, double value, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTDoubleArgumentAction* dAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTDoubleArgumentAction, NSLogEvents::EventKind::CreateNumberActionTag>(resultVarPtr);
        dAction->DoubleValue = value;
    }

    void EventLog::RecordJsRTCreateBoolean(Js::ScriptContext* ctx, bool value, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* bAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::CreateBooleanActionTag>(resultVarPtr);
        bAction->u_iVal = (value ? TRUE : FALSE);
    }

    void EventLog::RecordJsRTCreateString(Js::ScriptContext* ctx, const char16* stringValue, size_t stringLength, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTStringArgumentAction* sAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTStringArgumentAction, NSLogEvents::EventKind::CreateStringActionTag>(resultVarPtr);
        this->m_eventSlabAllocator.CopyStringIntoWLength(stringValue, (uint32)stringLength, sAction->StringValue);
    }

    void EventLog::RecordJsRTCreateSymbol(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* sAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::CreateSymbolActionTag>(resultVarPtr);
        sAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTVarToNumberConversion(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::VarConvertToNumberActionTag>(resultVarPtr);
        cAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTVarToBooleanConversion(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::VarConvertToBooleanActionTag>(resultVarPtr);
        cAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTVarToStringConversion(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::VarConvertToStringActionTag>(resultVarPtr);
        cAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTVarToObjectConversion(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::VarConvertToObjectActionTag>(resultVarPtr);
        cAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTAddRootRef(Js::ScriptContext* ctx, Js::Var var)
    {
        NSLogEvents::JsRTVarsArgumentAction* addAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::AddRootRefActionTag>();
        addAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTRemoveRootRef(Js::ScriptContext* ctx, Js::Var var)
    {
        NSLogEvents::JsRTVarsArgumentAction* removeAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::RemoveRootRefActionTag>();
        removeAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTEventLoopYieldPoint(Js::ScriptContext* ctx)
    {
        NSLogEvents::EventLoopYieldPointEntry* ypEvt = this->RecordGetInitializedEvent_Helper<NSLogEvents::EventLoopYieldPointEntry, NSLogEvents::EventKind::EventLoopYieldPointTag > ();
        ypEvt->EventTimeStamp = this->GetLastEventTime();
        ypEvt->EventWallTime = this->GetCurrentWallTime();

        //Put this here in the hope that after handling an event there is an idle period where we can work without blocking user work
        TTD::EventLog* elog = ctx->GetThreadContext()->TTDLog;
        if(elog->IsTimeForSnapshot())
        {
            BEGIN_ENTER_SCRIPT(ctx, true, true, true);
            {
                elog->PushMode(TTD::TTDMode::ExcludedExecution);
                elog->DoSnapshotExtract();
                elog->PruneLogLength();
                elog->PopMode(TTD::TTDMode::ExcludedExecution);
            }
            END_ENTER_SCRIPT;
        }
    }

    void EventLog::RecordJsRTAllocateBasicObject(Js::ScriptContext* ctx, TTDVar** resultVarPtr)
    {
        this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::AllocateObjectActionTag>(resultVarPtr);
    }

    void EventLog::RecordJsRTAllocateExternalObject(Js::ScriptContext* ctx, TTDVar** resultVarPtr)
    {
        this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::AllocateExternalObjectActionTag>(resultVarPtr);
    }

    void EventLog::RecordJsRTAllocateBasicArray(Js::ScriptContext* ctx, uint32 length, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::AllocateArrayActionTag>(resultVarPtr);
        cAction->u_iVal = length;
    }

    void EventLog::RecordJsRTAllocateArrayBuffer(Js::ScriptContext* ctx, uint32 size, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::AllocateArrayBufferActionTag>(resultVarPtr);
        cAction->u_iVal = size;
    }

    void EventLog::RecordJsRTAllocateExternalArrayBuffer(Js::ScriptContext* ctx, byte* buff, uint32 size, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTByteBufferAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTByteBufferAction, NSLogEvents::EventKind::AllocateExternalArrayBufferActionTag>(resultVarPtr);
        cAction->Length = size;

        cAction->Buffer = this->m_eventSlabAllocator.SlabAllocateArray<byte>(cAction->Length);
        js_memcpy_s(cAction->Buffer, cAction->Length, buff, size);
    }

    void EventLog::RecordJsRTAllocateFunction(Js::ScriptContext* ctx, bool isNamed, Js::Var optName, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* cAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::AllocateFunctionActionTag>(resultVarPtr);
        cAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(optName);
        cAction->u_bVal = isNamed ? TRUE : FALSE;
    }

    void EventLog::RecordJsRTGetAndClearException(Js::ScriptContext* ctx, TTDVar** resultVarPtr)
    {
        this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::GetAndClearExceptionActionTag>(resultVarPtr);
        //TODO: later we need to fill in additional the info for the action we want to track
    }

    void EventLog::RecordJsRTGetProperty(Js::ScriptContext* ctx, Js::PropertyId pid, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* gpAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::GetPropertyActionTag>(resultVarPtr);
        gpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        gpAction->u_pid = pid;
    }

    void EventLog::RecordJsRTGetIndex(Js::ScriptContext* ctx, Js::Var index, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* giAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::GetIndexActionTag>(resultVarPtr);
        giAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        giAction->Var2 = TTD_CONVERT_JSVAR_TO_TTDVAR(index);
    }

    void EventLog::RecordJsRTGetOwnPropertyInfo(Js::ScriptContext* ctx, Js::PropertyId pid, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* gpAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::GetOwnPropertyInfoActionTag>(resultVarPtr);
        gpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        gpAction->u_pid = pid;
    }

    void EventLog::RecordJsRTGetOwnPropertyNamesInfo(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* gpAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::GetOwnPropertyNamesInfoActionTag>(resultVarPtr);
        gpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTGetOwnPropertySymbolsInfo(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* gpAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::GetOwnPropertySymbolsInfoActionTag>(resultVarPtr);
        gpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTDefineProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, Js::Var propertyDescriptor)
    {
        NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction* dpAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsWithIntegralUnionArgumentAction, NSLogEvents::EventKind::DefinePropertyActionTag>();
        dpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        dpAction->Var2 = TTD_CONVERT_JSVAR_TO_TTDVAR(propertyDescriptor);
        dpAction->u_pid = pid;
    }

    void EventLog::RecordJsRTDeleteProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, bool useStrictRules, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction* dpAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction, NSLogEvents::EventKind::DeletePropertyActionTag>(resultVarPtr);
        dpAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        dpAction->Pid = pid;
        dpAction->BoolVal = useStrictRules ? TRUE : FALSE;
    }

    void EventLog::RecordJsRTSetPrototype(Js::ScriptContext* ctx, Js::Var var, Js::Var proto)
    {
        NSLogEvents::JsRTVarsArgumentAction* spAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::SetPrototypeActionTag>();
        spAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        spAction->Var2 = TTD_CONVERT_JSVAR_TO_TTDVAR(proto);
    }

    void EventLog::RecordJsRTSetProperty(Js::ScriptContext* ctx, Js::Var var, Js::PropertyId pid, Js::Var val, bool useStrictRules)
    {
        NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction* spAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsWithBoolAndPIDArgumentAction, NSLogEvents::EventKind::SetPropertyActionTag>();
        spAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        spAction->Var2 = TTD_CONVERT_JSVAR_TO_TTDVAR(val);
        spAction->Pid = pid;
        spAction->BoolVal = useStrictRules ? TRUE : FALSE;
    }

    void EventLog::RecordJsRTSetIndex(Js::ScriptContext* ctx, Js::Var var, Js::Var index, Js::Var val)
    {
        NSLogEvents::JsRTVarsArgumentAction* spAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::SetIndexActionTag>();
        spAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
        spAction->Var2 = TTD_CONVERT_JSVAR_TO_TTDVAR(index);
        spAction->Var3 = TTD_CONVERT_JSVAR_TO_TTDVAR(val);
    }

    void EventLog::RecordJsRTGetTypedArrayInfo(Js::ScriptContext* ctx, Js::Var var, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTVarsArgumentAction* giAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTVarsArgumentAction, NSLogEvents::EventKind::GetTypedArrayInfoActionTag>(resultVarPtr);
        giAction->Var1 = TTD_CONVERT_JSVAR_TO_TTDVAR(var);
    }

    void EventLog::RecordJsRTConstructCall(Js::ScriptContext* ctx, Js::JavascriptFunction* func, uint32 argCount, Js::Var* args, TTDVar** resultVarPtr)
    {
        NSLogEvents::JsRTConstructCallAction* ccAction = this->RecordGetInitializedEvent_HelperWithResultPtr<NSLogEvents::JsRTConstructCallAction, NSLogEvents::EventKind::ConstructCallActionTag>(resultVarPtr);
        
        ccAction->ArgCount = argCount + 1;

        static_assert(sizeof(TTDVar) == sizeof(Js::Var), "These need to be the same size (and have same bit layout) for this to work!");

        ccAction->ArgArray = this->m_eventSlabAllocator.SlabAllocateArray<TTDVar>(ccAction->ArgCount);
        ccAction->ArgArray[0] = TTD_CONVERT_JSVAR_TO_TTDVAR(func);
        js_memcpy_s(ccAction->ArgArray + 1, (ccAction->ArgCount - 1) * sizeof(TTDVar), args, argCount * sizeof(Js::Var));
    }

    void EventLog::RecordJsRTCallbackOperation(Js::ScriptContext* ctx, bool isCreate, bool isCancel, bool isRepeating, Js::JavascriptFunction* func, int64 callbackId)
    {
        NSLogEvents::JsRTCallbackAction* cbrAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTCallbackAction, NSLogEvents::EventKind::CallbackOpActionTag>();
        cbrAction->CurrentCallbackId = this->m_hostCallbackId;
        cbrAction->NewCallbackId = callbackId;

        //Register location is blank in record -- we only fill it in during debug replay

        cbrAction->IsCreate = isCreate;
        cbrAction->IsCancel = isCancel;
        cbrAction->IsRepeating = isRepeating;

        cbrAction->RegisterLocation = nullptr;
    }

    void EventLog::RecordJsRTCodeParse(Js::ScriptContext* ctx, uint64 bodyCtrId, LoadScriptFlag loadFlag, Js::JavascriptFunction* func, LPCWSTR srcCode, LPCWSTR sourceUri, Js::JavascriptFunction* resultFunction)
    {
        NSLogEvents::JsRTCodeParseAction* cpAction = this->RecordGetInitializedEvent_Helper<NSLogEvents::JsRTCodeParseAction, NSLogEvents::EventKind::CodeParseActionTag>();
        cpAction->AdditionalInfo = this->m_eventSlabAllocator.SlabAllocateStruct<NSLogEvents::JsRTCodeParseAction_AdditionalInfo>();

        cpAction->BodyCtrId = bodyCtrId;
        cpAction->Result = TTD_CONVERT_JSVAR_TO_TTDVAR(resultFunction);

        Js::FunctionBody* fb = JsSupport::ForceAndGetFunctionBody(func->GetFunctionBody());

        this->m_eventSlabAllocator.CopyNullTermStringInto(srcCode, cpAction->AdditionalInfo->SourceCode);

        this->m_eventSlabAllocator.CopyNullTermStringInto(fb->GetSourceContextInfo()->url, cpAction->AdditionalInfo->SourceUri);
        cpAction->AdditionalInfo->DocumentID = fb->GetUtf8SourceInfo()->GetSourceInfoId();

        cpAction->AdditionalInfo->LoadFlag = loadFlag;

        this->m_eventSlabAllocator.CopyStringIntoWLength(this->m_logInfoRootDir.Contents, this->m_logInfoRootDir.Length, cpAction->AdditionalInfo->SrcDir);
    }

    NSLogEvents::EventLogEntry* EventLog::RecordJsRTCallFunction(Js::ScriptContext* ctx, int32 rootDepth, int64 hostCallbackId, Js::JavascriptFunction* func, uint32 argCount, Js::Var* args)
    {
        NSLogEvents::EventLogEntry* evt = nullptr;
        this->RecordGetInitializedEvent_HelperWithMainEvent<NSLogEvents::JsRTCallFunctionAction, NSLogEvents::EventKind::CallExistingFunctionActionTag>(&evt);

        int64 evtTime = this->GetLastEventTime();
        int64 topLevelCallTime = (rootDepth == 0) ? evtTime : this->m_topLevelCallbackEventTime;
        double wallTime = this->m_timer.Now();
        NSLogEvents::JsRTCallFunctionAction_ProcessArgs(evt, rootDepth, evtTime, func, argCount, args, wallTime, hostCallbackId, topLevelCallTime, this->m_eventSlabAllocator);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        NSLogEvents::JsRTCallFunctionAction_ProcessDiagInfoPre(evt, func, this->m_eventSlabAllocator);
#endif

        return evt;
    }

    void EventLog::ReplayActionLoopRange(int64 eventTimeLimit)
    {
        AssertMsg(this->m_currentReplayEventIterator.IsValid() && this->m_currentReplayEventIterator.Current()->EventKind > NSLogEvents::EventKind::JsRTActionTag, "Should check this first!");
        AssertMsg(eventTimeLimit >= this->m_eventTimeCtr, "Why are we doing this then???");

        do
        {
            NSLogEvents::EventLogEntry* evt = this->m_currentReplayEventIterator.Current();
            this->AdvanceTimeAndPositionForReplay();

            auto executeFP = this->m_eventListVTable[(uint32)evt->EventKind].ExecuteFP;
            Js::ScriptContext* ctx = this->m_ttdContext;
            if(NSLogEvents::IsJsRTActionExecutedInScriptWrapper(evt->EventKind))
            {
                BEGIN_ENTER_SCRIPT(ctx, true, true, true);
                {
                    executeFP(evt, ctx);
                }
                END_ENTER_SCRIPT;
            }
            else
            {
                executeFP(evt, ctx);
            }

        } while(eventTimeLimit >= this->m_eventTimeCtr);
    }

    LPCWSTR EventLog::EmitLogIfNeeded()
    {
        //See if we have been running record mode (even if we are suspended for runtime execution) -- if we aren't then we don't want to emit anything
        if((this->m_currentMode & TTDMode::RecordEnabled) != TTDMode::RecordEnabled)
        {
            return _u("Record Disabled -- No Log Written!");
        }

#if ENABLE_BASIC_TRACE || ENABLE_FULL_BC_TRACE
        this->m_diagnosticLogger.ForceFlush();
#endif

        HANDLE logHandle = this->m_threadContext->TTDStreamFunctions.pfGetLogStream(this->m_logInfoRootDir.Contents, false, true);
        TTD_LOG_WRITER writer(logHandle, TTD_COMPRESSED_OUTPUT, this->m_threadContext->TTDStreamFunctions.pfWriteBytesToStream, this->m_threadContext->TTDStreamFunctions.pfFlushAndCloseStream);

        writer.WriteRecordStart();
        writer.AdjustIndent(1);

        TTString archString;
#if defined(_M_IX86)
        this->m_miscSlabAllocator.CopyNullTermStringInto(_u("x86"), archString);
#elif defined(_M_X64)
        this->m_miscSlabAllocator.CopyNullTermStringInto(_u("x64"), archString);
#elif defined(_M_ARM)
        this->m_miscSlabAllocator.CopyNullTermStringInto(_u("arm"), archString);
#elif defined(_M_ARM64)
        this->m_miscSlabAllocator.CopyNullTermStringInto(_u("arm64"), archString);
#else
        this->m_miscSlabAllocator.CopyNullTermStringInto(_u("unknown"), archString);
#endif

        writer.WriteString(NSTokens::Key::arch, archString);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        bool diagEnabled = true;
#else
        bool diagEnabled = false;
#endif

        writer.WriteBool(NSTokens::Key::diagEnabled, diagEnabled, NSTokens::Separator::CommaSeparator);

        uint64 usedSpace = 0;
        uint64 reservedSpace = 0;
        this->m_eventSlabAllocator.ComputeMemoryUsed(&usedSpace, &reservedSpace);

        writer.WriteUInt64(NSTokens::Key::usedMemory, usedSpace, NSTokens::Separator::CommaSeparator);
        writer.WriteUInt64(NSTokens::Key::reservedMemory, reservedSpace, NSTokens::Separator::CommaSeparator);

        uint32 ecount = this->m_eventList.Count();
        writer.WriteLengthValue(ecount, NSTokens::Separator::CommaAndBigSpaceSeparator);

        JsUtil::Stack<int64, HeapAllocator> callNestingStack(&HeapAllocator::Instance);
        bool firstElem = true;

        writer.WriteSequenceStart_DefaultKey(NSTokens::Separator::CommaSeparator);
        writer.AdjustIndent(1);
        writer.WriteSeperator(NSTokens::Separator::BigSpaceSeparator);
        for(auto iter = this->m_eventList.GetIteratorAtFirst(); iter.IsValid(); iter.MoveNext())
        {
            const NSLogEvents::EventLogEntry* evt = iter.Current();

            NSTokens::Separator sep = firstElem ? NSTokens::Separator::NoSeparator : NSTokens::Separator::BigSpaceSeparator;
            NSLogEvents::EventLogEntry_Emit(evt, this->m_eventListVTable, &writer, this->m_logInfoRootDir.Contents, this->m_threadContext, sep);

            firstElem = false;
#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
            bool isJsRTCall = (evt->EventKind == NSLogEvents::EventKind::CallExistingFunctionActionTag);
            bool isExternalCall = (evt->EventKind == NSLogEvents::EventKind::ExternalCallTag);
            bool isRegisterCall = (evt->EventKind == NSLogEvents::EventKind::ExternalCbRegisterCall);
            if(isJsRTCall | isExternalCall | isRegisterCall)
            {
                writer.WriteSequenceStart(NSTokens::Separator::BigSpaceSeparator);

                int64 lastNestedTime = -1;
                if(isJsRTCall)
                {
                    lastNestedTime = NSLogEvents::JsRTCallFunctionAction_GetLastNestedEventTime(evt);
                }
                else if(isExternalCall)
                {
                    lastNestedTime = NSLogEvents::ExternalCallEventLogEntry_GetLastNestedEventTime(evt);
                }
                else
                {
                    lastNestedTime = NSLogEvents::ExternalCbRegisterCallEventLogEntry_GetLastNestedEventTime(evt);
                }
                callNestingStack.Push(lastNestedTime);

                if(lastNestedTime != evt->EventTimeStamp)
                {
                    writer.AdjustIndent(1);

                    writer.WriteSeperator(NSTokens::Separator::BigSpaceSeparator);
                    firstElem = true;
                }
            }

            if(callNestingStack.Count() > 0 && evt->EventTimeStamp == callNestingStack.Peek())
            {
                int64 eTime = callNestingStack.Pop();

                if(!isJsRTCall & !isExternalCall & !isRegisterCall)
                {
                    writer.AdjustIndent(-1);
                    writer.WriteSeperator(NSTokens::Separator::BigSpaceSeparator);
                }
                writer.WriteSequenceEnd();

                while(callNestingStack.Count() > 0 && eTime == callNestingStack.Peek())
                {
                    callNestingStack.Pop();

                    writer.AdjustIndent(-1);
                    writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);
                }
            }
#endif
        }
        writer.AdjustIndent(-1);
        writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);

        //we haven't moved the properties to their serialized form them take care of it 
        AssertMsg(this->m_propertyRecordList.Count() == 0, "We only compute this when we are ready to emit.");

        for(auto iter = this->m_propertyRecordPinSet->GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            Js::PropertyRecord* pRecord = static_cast<Js::PropertyRecord*>(iter.CurrentValue());
            NSSnapType::SnapPropertyRecord* sRecord = this->m_propertyRecordList.NextOpenEntry();

            sRecord->PropertyId = pRecord->GetPropertyId();
            sRecord->IsNumeric = pRecord->IsNumeric();
            sRecord->IsBound = pRecord->IsBound();
            sRecord->IsSymbol = pRecord->IsSymbol();

            this->m_miscSlabAllocator.CopyStringIntoWLength(pRecord->GetBuffer(), pRecord->GetLength(), sRecord->PropertyName);
        }

        //emit the properties
        writer.WriteLengthValue(this->m_propertyRecordList.Count(), NSTokens::Separator::CommaSeparator);

        writer.WriteSequenceStart_DefaultKey(NSTokens::Separator::CommaSeparator);
        writer.AdjustIndent(1);
        bool firstProperty = true;
        for(auto iter = this->m_propertyRecordList.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            NSTokens::Separator sep = (!firstProperty) ? NSTokens::Separator::CommaAndBigSpaceSeparator : NSTokens::Separator::BigSpaceSeparator;
            NSSnapType::EmitSnapPropertyRecord(iter.Current(), &writer, sep);

            firstProperty = false;
        }
        writer.AdjustIndent(-1);
        writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);

        //do top level script processing here
        writer.WriteLengthValue(this->m_loadedTopLevelScripts.Count(), NSTokens::Separator::CommaSeparator);
        writer.WriteSequenceStart_DefaultKey(NSTokens::Separator::CommaSeparator);
        writer.AdjustIndent(1);
        bool firstLoadScript = true;
        for(auto iter = this->m_loadedTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            NSTokens::Separator sep = (!firstLoadScript) ? NSTokens::Separator::CommaAndBigSpaceSeparator : NSTokens::Separator::BigSpaceSeparator;
            NSSnapValues::EmitTopLevelLoadedFunctionBodyInfo(iter.Current(), this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &writer, sep);

            firstLoadScript = false;
        }
        writer.AdjustIndent(-1);
        writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);

        writer.WriteLengthValue(this->m_newFunctionTopLevelScripts.Count(), NSTokens::Separator::CommaSeparator);
        writer.WriteSequenceStart_DefaultKey(NSTokens::Separator::CommaSeparator);
        writer.AdjustIndent(1);
        bool firstNewScript = true;
        for(auto iter = this->m_newFunctionTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            NSTokens::Separator sep = (!firstNewScript) ? NSTokens::Separator::CommaAndBigSpaceSeparator : NSTokens::Separator::BigSpaceSeparator;
            NSSnapValues::EmitTopLevelNewFunctionBodyInfo(iter.Current(), this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &writer, sep);

            firstNewScript = false;
        }
        writer.AdjustIndent(-1);
        writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);

        writer.WriteLengthValue(this->m_evalTopLevelScripts.Count(), NSTokens::Separator::CommaSeparator);
        writer.WriteSequenceStart_DefaultKey(NSTokens::Separator::CommaSeparator);
        writer.AdjustIndent(1);
        bool firstEvalScript = true;
        for(auto iter = this->m_evalTopLevelScripts.GetIterator(); iter.IsValid(); iter.MoveNext())
        {
            NSTokens::Separator sep = (!firstEvalScript) ? NSTokens::Separator::CommaAndBigSpaceSeparator : NSTokens::Separator::BigSpaceSeparator;
            NSSnapValues::EmitTopLevelEvalFunctionBodyInfo(iter.Current(), this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &writer, sep);

            firstEvalScript = false;
        }
        writer.AdjustIndent(-1);
        writer.WriteSequenceEnd(NSTokens::Separator::BigSpaceSeparator);
        //

        writer.AdjustIndent(-1);
        writer.WriteRecordEnd(NSTokens::Separator::BigSpaceSeparator);

        writer.FlushAndClose();

        return this->m_logInfoRootDir.Contents;
    }

    void EventLog::ParseLogInto()
    {
        HANDLE logHandle = this->m_threadContext->TTDStreamFunctions.pfGetLogStream(this->m_logInfoRootDir.Contents, true, false);
        TTD_LOG_READER reader(logHandle, TTD_COMPRESSED_OUTPUT, this->m_threadContext->TTDStreamFunctions.pfReadBytesFromStream, this->m_threadContext->TTDStreamFunctions.pfFlushAndCloseStream);

        reader.ReadRecordStart();

        TTString archString;
        reader.ReadString(NSTokens::Key::arch, this->m_miscSlabAllocator, archString);

#if defined(_M_IX86)
        AssertMsg(wcscmp(_u("x86"), archString.Contents) == 0, "Mismatch in arch between record and replay!!!");
#elif defined(_M_X64)
        AssertMsg(wcscmp(_u("x64"), archString.Contents) == 0, "Mismatch in arch between record and replay!!!");
#elif defined(_M_ARM)
        AssertMsg(wcscmp(_u("arm64"), archString.Contents) == 0, "Mismatch in arch between record and replay!!!");
#else
        AssertMsg(false, "Unknown arch!!!");
#endif

        bool diagEnabled = reader.ReadBool(NSTokens::Key::diagEnabled, true);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        AssertMsg(diagEnabled, "Diag was enabled in record so it shoud be in replay as well!!!");
#else
        AssertMsg(!diagEnabled, "Diag was *not* enabled in record so it shoud *not* be in replay either!!!");
#endif

        reader.ReadUInt64(NSTokens::Key::usedMemory, true);
        reader.ReadUInt64(NSTokens::Key::reservedMemory, true);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
        JsUtil::Stack<int64, HeapAllocator> callNestingStack(&HeapAllocator::Instance);
        bool doSep = false;
#endif

        uint32 ecount = reader.ReadLengthValue(true);
        reader.ReadSequenceStart_WDefaultKey(true);
        for(uint32 i = 0; i < ecount; ++i)
        {
            NSLogEvents::EventLogEntry* evt = this->m_eventList.GetNextAvailableEntry();
            NSLogEvents::EventLogEntry_Parse(evt, this->m_eventListVTable, false, this->m_threadContext, &reader, this->m_eventSlabAllocator);

#if ENABLE_TTD_INTERNAL_DIAGNOSTICS
            bool isJsRTCall = (evt->EventKind == NSLogEvents::EventKind::CallExistingFunctionActionTag);
            bool isExternalCall = (evt->EventKind == NSLogEvents::EventKind::ExternalCallTag);
            bool isRegisterCall = (evt->EventKind == NSLogEvents::EventKind::ExternalCbRegisterCall);
            if(isJsRTCall | isExternalCall | isRegisterCall)
            {
                reader.ReadSequenceStart(false);

                int64 lastNestedTime = -1;
                if(isJsRTCall)
                {
                    lastNestedTime = NSLogEvents::JsRTCallFunctionAction_GetLastNestedEventTime(evt);
                }
                else if(isExternalCall)
                {
                    lastNestedTime = NSLogEvents::ExternalCallEventLogEntry_GetLastNestedEventTime(evt);
                }
                else
                {
                    lastNestedTime = NSLogEvents::ExternalCbRegisterCallEventLogEntry_GetLastNestedEventTime(evt);
                }
                callNestingStack.Push(lastNestedTime);
            }

            doSep = (!isJsRTCall & !isExternalCall & !isRegisterCall);

            while(callNestingStack.Count() > 0 && evt->EventTimeStamp == callNestingStack.Peek())
            {
                callNestingStack.Pop();
                reader.ReadSequenceEnd();
            }
#endif
        }
        reader.ReadSequenceEnd();

        //parse the properties
        uint32 propertyCount = reader.ReadLengthValue(true);
        reader.ReadSequenceStart_WDefaultKey(true);
        for(uint32 i = 0; i < propertyCount; ++i)
        {
            NSSnapType::SnapPropertyRecord* sRecord = this->m_propertyRecordList.NextOpenEntry();
            NSSnapType::ParseSnapPropertyRecord(sRecord, i != 0, &reader, this->m_miscSlabAllocator);
        }
        reader.ReadSequenceEnd();

        //do top level script processing here
        uint32 loadedScriptCount = reader.ReadLengthValue(true);
        reader.ReadSequenceStart_WDefaultKey(true);
        for(uint32 i = 0; i < loadedScriptCount; ++i)
        {
            NSSnapValues::TopLevelScriptLoadFunctionBodyResolveInfo* fbInfo = this->m_loadedTopLevelScripts.NextOpenEntry();
            NSSnapValues::ParseTopLevelLoadedFunctionBodyInfo(fbInfo, i != 0, this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &reader, this->m_miscSlabAllocator);
        }
        reader.ReadSequenceEnd();

        uint32 newScriptCount = reader.ReadLengthValue(true);
        reader.ReadSequenceStart_WDefaultKey(true);
        for(uint32 i = 0; i < newScriptCount; ++i)
        {
            NSSnapValues::TopLevelNewFunctionBodyResolveInfo* fbInfo = this->m_newFunctionTopLevelScripts.NextOpenEntry();
            NSSnapValues::ParseTopLevelNewFunctionBodyInfo(fbInfo, i != 0, this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &reader, this->m_miscSlabAllocator);
        }
        reader.ReadSequenceEnd();

        uint32 evalScriptCount = reader.ReadLengthValue(true);
        reader.ReadSequenceStart_WDefaultKey(true);
        for(uint32 i = 0; i < evalScriptCount; ++i)
        {
            NSSnapValues::TopLevelEvalFunctionBodyResolveInfo* fbInfo = this->m_evalTopLevelScripts.NextOpenEntry();
            NSSnapValues::ParseTopLevelEvalFunctionBodyInfo(fbInfo, i != 0, this->m_logInfoRootDir.Contents, this->m_threadContext->TTDStreamFunctions, &reader, this->m_miscSlabAllocator);
        }
        reader.ReadSequenceEnd();
        //

        reader.ReadRecordEnd();
    }
}

#endif
