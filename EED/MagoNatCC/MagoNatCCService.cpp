// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// _MagoNatCCService.cpp : Implementation of CMagoNatCCService

#include "stdafx.h"
#include "MagoNatCCService.h"
#include <dia2.h>
#include <msdbg.h>

#include "Common.h"
#include "EED.h"
#include "UniAlpha.h"
#include "Guard.h"

#include "../../CVSym/CVSym/CVSymPublic.h"
#include "../../CVSym/CVSym/Error.h"
#include "../../CVSym/CVSym/ISymbolInfo.h"

#include "../../CVSym/CVSTI/CVSTIPublic.h"
#include "../../CVSym/CVSTI/IDataSource.h"
#include "../../CVSym/CVSTI/ISession.h"
#include "../../CVSym/CVSTI/ImageAddrMap.h"

#include "../../DebugEngine/Include/MagoDECommon.h"
#include "../../DebugEngine/Include/WinPlat.h"
#include "../../DebugEngine/MagoNatDE/Utility.h"
#include "../../DebugEngine/MagoNatDE/ExprContext.h"
#include "../../DebugEngine/MagoNatDE/Property.h"
#include "../../DebugEngine/MagoNatDE/DRuntime.h"
#include "../../DebugEngine/MagoNatDE/RegisterSet.h"
#include "../../DebugEngine/MagoNatDE/ArchDataX64.h"
#include "../../DebugEngine/MagoNatDE/ArchDataX86.h"
#include "../../DebugEngine/MagoNatDE/EnumPropertyInfo.h"
#include "../../DebugEngine/MagoNatDE/FrameProperty.h"
#include "../../DebugEngine/MagoNatDE/CodeContext.h"
#include "../../DebugEngine/MagoNatDE/Module.h"
#include "../../DebugEngine/Exec/Types.h"
#include "../../DebugEngine/Exec/Exec.h"
#include "../../DebugEngine/Exec/Thread.h"
#include "../../DebugEngine/Exec/IProcess.h"
#include "../../DebugEngine/Exec/IModule.h"
#include "../../DebugEngine/MagoNatDE/IDebuggerProxy.h"
#include "../../DebugEngine/MagoNatDE/Thread.h"
#include "../../DebugEngine/MagoNatDE/Program.h"
#include "../../DebugEngine/MagoNatDE/LocalProcess.h"

///////////////////////////////////////////////////////////////////////////////
#define NOT_IMPL(x) virtual HRESULT x override { return E_NOTIMPL; }

#define tryHR(x) do { HRESULT _hr = (x); if(FAILED(_hr)) return _hr; } while(false)

// stub to read/write memory from process, all other functions supposed to not be called
class CCDebuggerProxy : public Mago::IDebuggerProxy
{
    RefPtr<DkmProcess> mProcess;

public:
    CCDebuggerProxy(DkmProcess* process)
        : mProcess(process)
    {
    }

    NOT_IMPL( Launch( LaunchInfo* launchInfo, Mago::ICoreProcess*& process ) );
    NOT_IMPL( Attach( uint32_t id, Mago::ICoreProcess*& process ) );

    NOT_IMPL( Terminate( Mago::ICoreProcess* process ) );
    NOT_IMPL( Detach( Mago::ICoreProcess* process ) );

    NOT_IMPL( ResumeLaunchedProcess( Mago::ICoreProcess* process ) );

    virtual HRESULT ReadMemory( 
        Mago::ICoreProcess* process, 
        Mago::Address64 address,
        uint32_t length, 
        uint32_t& lengthRead, 
        uint32_t& lengthUnreadable, 
        uint8_t* buffer )
    {
        // assume single process for now
        tryHR(mProcess->ReadMemory(address, DkmReadMemoryFlags::None, buffer, length, &lengthRead));
        lengthUnreadable = length - lengthRead;
        return S_OK;
    }

    virtual HRESULT WriteMemory( 
        Mago::ICoreProcess* process, 
        Mago::Address64 address,
        uint32_t length, 
        uint32_t& lengthWritten, 
        uint8_t* buffer )
    {
        // assume single process for now
        DkmArray<BYTE> arr = { buffer, length };
        tryHR(mProcess->WriteMemory(address, arr));
        lengthWritten = length;
        return S_OK;
    }

    NOT_IMPL( SetBreakpoint( Mago::ICoreProcess* process, Mago::Address64 address ) );
    NOT_IMPL( RemoveBreakpoint( Mago::ICoreProcess* process, Mago::Address64 address ) );

    NOT_IMPL( StepOut( Mago::ICoreProcess* process, Mago::Address64 targetAddr, bool handleException ) );
    NOT_IMPL( StepInstruction( Mago::ICoreProcess* process, bool stepIn, bool handleException ) );
    NOT_IMPL( StepRange( 
        Mago::ICoreProcess* process, bool stepIn, Mago::AddressRange64 range, bool handleException ) );

    NOT_IMPL( Continue( Mago::ICoreProcess* process, bool handleException ) );
    NOT_IMPL( Execute( Mago::ICoreProcess* process, bool handleException ) );

    NOT_IMPL( AsyncBreak( Mago::ICoreProcess* process ) );

    NOT_IMPL( GetThreadContext( Mago::ICoreProcess* process, Mago::ICoreThread* thread, Mago::IRegisterSet*& regSet ) );
    NOT_IMPL( SetThreadContext( Mago::ICoreProcess* process, Mago::ICoreThread* thread, Mago::IRegisterSet* regSet ) );

    NOT_IMPL( GetPData( 
        Mago::ICoreProcess* process, 
        Mago::Address64 address, 
        Mago::Address64 imageBase, 
        uint32_t size, 
        uint32_t& sizeRead, 
        uint8_t* pdata ) );
};

class CCCoreModule : public Mago::ICoreModule
{
public:
    CCCoreModule(DkmModuleInstance* modInst) : mModule(modInst) { }

    virtual void AddRef()
    {
        InterlockedIncrement(&mRefCount);
    }
    virtual void Release()
    {
        long newRef = InterlockedDecrement(&mRefCount);
        _ASSERT(newRef >= 0);
        if (newRef == 0)
            delete this;
    }

	// Program needs the image base
    virtual Mago::Address64 GetImageBase() { return mModule->BaseAddress(); }
    virtual Mago::Address64 GetPreferredImageBase() { return mModule->BaseAddress(); }
    virtual uint32_t        GetSize() { return mModule->Size(); }
    virtual uint16_t        GetMachine() { return 0; }
    virtual const wchar_t*  GetPath() { return mModule->FullName()->Value(); }
    virtual const wchar_t*  GetSymbolSearchPath() { return mModule->FullName()->Value(); }

private:
    long mRefCount = 0;

    RefPtr<DkmModuleInstance> mModule;
};

///////////////////////////////////////////////////////////////////////////////
template<typename I>
I* GetEnumTable(IDiaSession *pSession)
{
    I*              pUnknown    = NULL;
    REFIID          iid         = __uuidof(I);
    IDiaEnumTables* pEnumTables = NULL;
    IDiaTable*      pTable      = NULL;
    ULONG           celt        = 0;

    if (pSession->getEnumTables(&pEnumTables) != S_OK)
    {
        // wprintf(L"ERROR - GetTable() getEnumTables\n");
        return NULL;
    }
    while (pEnumTables->Next(1, &pTable, &celt) == S_OK && celt == 1)
    {
        // There is only one table that matches the given iid
        HRESULT hr = pTable->QueryInterface(iid, (void**)&pUnknown);
        pTable->Release();
        if (hr == S_OK)
            break;
    }
    pEnumTables->Release();
    return pUnknown;
}

///////////////////////////////////////////////////////////////////////////////
template<class T>
class CCDataItem : public IUnknown
{
public:
    // COM like ref counting
    virtual ULONG STDMETHODCALLTYPE AddRef()
    {
        long newRef = InterlockedIncrement(&mRefCount);
        return newRef;
    }
    virtual ULONG STDMETHODCALLTYPE Release()
    {
        long newRef = InterlockedDecrement(&mRefCount);
        _ASSERT(newRef >= 0);
        if (newRef == 0)
            delete this;
        return newRef;
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if (riid == __uuidof(IUnknown))
        {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        else if (riid == __uuidof(T))
        {
            *ppv = static_cast<T*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

private:
    long mRefCount = 0;
};

///////////////////////////////////////////////////////////////////////////////
static Mago::ProcFeaturesX86 toMago(DefaultPort::DkmProcessorFeatures::e features)
{
    int flags = Mago::PF_X86_None;
    if (features & DefaultPort::DkmProcessorFeatures::MMX)
        flags |= Mago::PF_X86_MMX;
    if (features & DefaultPort::DkmProcessorFeatures::SSE)
        flags |= Mago::PF_X86_SSE;
    if (features & DefaultPort::DkmProcessorFeatures::SSE2)
        flags |= Mago::PF_X86_SSE2;
    if (features & DefaultPort::DkmProcessorFeatures::AMD3DNow)
        flags |= Mago::PF_X86_3DNow;
    if (features & DefaultPort::DkmProcessorFeatures::AVX)
        flags |= Mago::PF_X86_AVX;
    if (features & DefaultPort::DkmProcessorFeatures::VFP32)
        flags |= Mago::PF_X86_None; // not an x86 feature

    return Mago::ProcFeaturesX86(flags);
}

// dia2.h has the wrong IID?
// {2F609EE1-D1C8-4E24-8288-3326BADCD211}
EXTERN_C const GUID DECLSPEC_SELECTANY guidIDiaSession =
{ 0x2F609EE1, 0xD1C8, 0x4E24, { 0x82, 0x88, 0x33, 0x26, 0xBA, 0xDC, 0xD2, 0x11 } };

class DECLSPEC_UUID("598DECC9-CF79-4E90-A408-5E1433B4DBFF") CCModule : public CCDataItem<CCModule>
{
    friend class CCExprContext;

protected:
    CCModule() : mDRuntime(nullptr), mDebuggerProxy(nullptr) {}
    ~CCModule() 
    {
        delete mDRuntime;
        delete mDebuggerProxy;
    }

    RefPtr<MagoST::IDataSource> mDataSource;
    RefPtr<MagoST::ISession> mSession;
    RefPtr<MagoST::ImageAddrMap> mAddrMap;
    RefPtr<Mago::ArchData> mArchData;
    RefPtr<Mago::IRegisterSet> mRegSet;
    RefPtr<Mago::Module> mModule;
    RefPtr<Mago::Program> mProgram;

    CCDebuggerProxy* mDebuggerProxy;
    Mago::DRuntime* mDRuntime;
    RefPtr<DkmProcess> mProcess;

    HRESULT Init(DkmProcess* process, Symbols::DkmModule* module)
    {
        auto system = process->SystemInformation();
        auto arch = system->ProcessorArchitecture();
        int ptrSize = arch == PROCESSOR_ARCHITECTURE_AMD64 ? 8 : 4;

        CComPtr<IDiaSession> diasession;
        if (module->GetSymbolInterface(guidIDiaSession, (IUnknown**)&diasession) != S_OK) // VS 2015
            if (HRESULT hr = module->GetSymbolInterface(__uuidof(IDiaSession), (IUnknown**)&diasession) != S_OK) // VS2013
                return hr;

        auto features = toMago(system->ProcessorFeatures());
        if (arch == PROCESSOR_ARCHITECTURE_AMD64)
            mArchData = new Mago::ArchDataX64(features);
        else
            mArchData = new Mago::ArchDataX86(features);

        CONTEXT_X64 context;
        tryHR(mArchData->BuildRegisterSet(&context, sizeof(context), mRegSet.Ref()));

        mDebuggerProxy = new CCDebuggerProxy(process);
        mProcess = process;
        mAddrMap = new MagoST::ImageAddrMap;
        tryHR(mAddrMap->LoadFromDiaSession(diasession));

        tryHR(MakeCComObject(mModule));
        tryHR(MagoST::MakeDataSource(mDataSource.Ref()));
        tryHR(mDataSource->InitDebugInfo(diasession, mAddrMap));
        tryHR(mDataSource->OpenSession(mSession.Ref()));
        mModule->SetSession(mSession);
		DkmArray<Microsoft::VisualStudio::Debugger::DkmModuleInstance*> pModules = { 0 };
        module->GetModuleInstances(&pModules);
        if(pModules.Length > 0)
            mModule->SetCoreModule(new CCCoreModule(pModules.Members[0]));
		DkmFreeArray(pModules);

        tryHR(MakeCComObject(mProgram));

        // any process to pass architecture info
        RefPtr<Mago::LocalProcess> localprocess = new Mago::LocalProcess(mArchData);
        mProgram->SetCoreProcess(localprocess);
        mProgram->SetDebuggerProxy(mDebuggerProxy);
        mProgram->AddModule(mModule);
        UniquePtr<Mago::DRuntime> druntime(new Mago::DRuntime(mDebuggerProxy, localprocess));
        mProgram->SetDRuntime(druntime);

        return S_OK;
    }

    HRESULT FindFunction(uint32_t rva, MagoST::SymHandle& funcSH, std::vector<MagoST::SymHandle>& blockSH)
    {
        uint32_t    offset = 0;
        uint16_t    sec = mSession->GetSecOffsetFromRVA( rva, offset );
        if ( sec == 0 )
            return E_NOT_FOUND;

        // TODO: verify that it's a function or public symbol (or something else?)
        HRESULT hr = mSession->FindOuterSymbolByAddr( MagoST::SymHeap_GlobalSymbols, sec, offset, funcSH );
        if ( FAILED( hr ) )
            hr = mSession->FindOuterSymbolByAddr( MagoST::SymHeap_StaticSymbols, sec, offset, funcSH );
        if ( FAILED( hr ) )
            hr = mSession->FindOuterSymbolByAddr( MagoST::SymHeap_PublicSymbols, sec, offset, funcSH );
        if ( FAILED( hr ) )
            return hr;

        hr = mSession->FindInnermostSymbol( funcSH, sec, offset, blockSH );
        // it might be a public symbol, which doesn't have anything: blocks, args, or locals

        return S_OK;
    }

public:
    virtual HRESULT ReadMemory(MagoEE::Address addr, uint32_t sizeToRead, uint32_t& sizeRead, uint8_t* buffer)
    {
        return mProcess->ReadMemory(addr, DkmReadMemoryFlags::None, buffer, sizeToRead, &sizeRead);
    }
};

///////////////////////////////////////////////////////////////////////////////
class CCExprContext : public Mago::ExprContext
{
    RefPtr<CallStack::DkmStackWalkFrame> mStackFrame;
    RefPtr<CCModule> mModule;
    RefPtr<Mago::Thread> mThread;

public:
    CCExprContext() {}
    ~CCExprContext() 
    {
    }
    
    HRESULT Init(DkmProcess* process, CallStack::DkmStackWalkFrame* frame)
    {
        HRESULT hr;
        CComPtr<Symbols::DkmInstructionSymbol> pInstruction;
        hr = frame->GetInstructionSymbol(&pInstruction);
        if (hr != S_OK)
          return hr;

        auto nativeInst = Native::DkmNativeInstructionSymbol::TryCast(pInstruction);
        if (!nativeInst)
          return S_FALSE;

        Symbols::DkmModule* module = pInstruction->Module();
        if (!module)
          return S_FALSE;

        // Restore/Create CCModule in DkmModule
        hr = module->GetDataItem(&mModule.Ref());
        if (!mModule)
        {
            mModule = new CCModule; 
            tryHR(mModule->Init(process, module));
            tryHR(module->SetDataItem(DkmDataCreationDisposition::CreateAlways, mModule.Get()));
        }

        mStackFrame = frame;

        uint32_t rva = nativeInst->RVA();
        MagoST::SymHandle funcSH;
        std::vector<MagoST::SymHandle> blockSH;
        tryHR(mModule->FindFunction(rva, funcSH, blockSH));

        // setup a fake environment for Mago
        tryHR(MakeCComObject(mThread));
        mThread->SetProgram(mModule->mProgram, mModule->mDebuggerProxy);

        return ExprContext::Init(mModule->mModule, mThread, funcSH, blockSH, /*image-base+*/rva, mModule->mRegSet);
    }

    virtual HRESULT GetSession(MagoST::ISession*& session)
    {
        session = mModule->mSession;
        session->AddRef();
        return S_OK;
    }

    virtual HRESULT GetRegValue(DWORD reg, MagoEE::DataValueKind& kind, MagoEE::DataValue& value)
    {
        if (auto regs = mStackFrame->Registers())
        {
            UINT32 read;
            Mago::RegisterValue regVal = { 0 };
            tryHR(regs->GetRegisterValue(reg, &regVal.Value, sizeof (regVal.Value), &read));

            int archRegId = mModule->mArchData->GetArchRegId( reg );
            if ( archRegId < 0 )
                return E_NOT_FOUND;

            regVal.Type = mModule->mRegSet->GetRegisterType( archRegId );
            switch ( regVal.Type )
            {
            case Mago::RegType_Int8:  value.UInt64Value = regVal.Value.I8;    kind = MagoEE::DataValueKind_UInt64;    break;
            case Mago::RegType_Int16: value.UInt64Value = regVal.Value.I16;   kind = MagoEE::DataValueKind_UInt64;    break;
            case Mago::RegType_Int32: value.UInt64Value = regVal.Value.I32;   kind = MagoEE::DataValueKind_UInt64;    break;
            case Mago::RegType_Int64: value.UInt64Value = regVal.Value.I64;   kind = MagoEE::DataValueKind_UInt64;    break;

            case Mago::RegType_Float32:
                value.Float80Value.FromFloat( regVal.Value.F32 );
                kind = MagoEE::DataValueKind_Float80;
                break;

            case Mago::RegType_Float64:
                value.Float80Value.FromDouble( regVal.Value.F64 );
                kind = MagoEE::DataValueKind_Float80;
                break;

            case Mago::RegType_Float80:   
                memcpy( &value.Float80Value, regVal.Value.F80Bytes, sizeof value.Float80Value );
                kind = MagoEE::DataValueKind_Float80;
                break;

            default:
                return E_FAIL;
            }
            return S_OK;
        }
        return E_FAIL;
    }

    virtual Mago::Address64 GetTebBase()
    {
        return mStackFrame->Thread()->TebAddress();
    }

    Mago::IRegisterSet* getRegSet() { return mModule->mRegSet; }

    // COM like ref counting
    virtual ULONG STDMETHODCALLTYPE AddRef()
    {
        long newRef = InterlockedIncrement( &mRefCount );
        return newRef;
    }
    virtual ULONG STDMETHODCALLTYPE Release()
    {
        long newRef = InterlockedDecrement( &mRefCount );
        _ASSERT( newRef >= 0 );
        if ( newRef == 0 )
            delete this;
        return newRef;
    }

private:
    long mRefCount = 0;
};

///////////////////////////////////////////////////////////////////////////////
CComPtr<DkmString> toDkmString(const wchar_t* str)
{
    CComPtr<DkmString> pString;
    HRESULT hr = DkmString::Create(str, &pString);
    _ASSERT(hr == S_OK);
    return pString;
}

///////////////////////////////////////////////////////////////////////////////
Evaluation::DkmEvaluationResultFlags::e toResultFlags(DBG_ATTRIB_FLAGS attrib)
{
    Evaluation::DkmEvaluationResultFlags::e resultFlags = Evaluation::DkmEvaluationResultFlags::None;
    if ( attrib & DBG_ATTRIB_VALUE_RAW_STRING )
        resultFlags |= Evaluation::DkmEvaluationResultFlags::RawString;
    if ( attrib & DBG_ATTRIB_VALUE_READONLY )
        resultFlags |= Evaluation::DkmEvaluationResultFlags::ReadOnly;
    if ( attrib & DBG_ATTRIB_OBJ_IS_EXPANDABLE )
        resultFlags |= Evaluation::DkmEvaluationResultFlags::Expandable;
    return resultFlags;
}

Evaluation::DkmEvaluationResultAccessType::e toResultAccess(DBG_ATTRIB_FLAGS attrib)
{
    Evaluation::DkmEvaluationResultAccessType::e resultAccess = Evaluation::DkmEvaluationResultAccessType::None;
    if ( attrib & DBG_ATTRIB_ACCESS_PUBLIC )
        resultAccess = Evaluation::DkmEvaluationResultAccessType::Public;
    else if ( attrib & DBG_ATTRIB_ACCESS_PRIVATE )
        resultAccess = Evaluation::DkmEvaluationResultAccessType::Private;
    else if ( attrib & DBG_ATTRIB_ACCESS_PROTECTED )
        resultAccess = Evaluation::DkmEvaluationResultAccessType::Protected;
    else if ( attrib & DBG_ATTRIB_ACCESS_FINAL )
        resultAccess = Evaluation::DkmEvaluationResultAccessType::Final;
    return resultAccess;
}

Evaluation::DkmEvaluationResultStorageType::e toResultStorage(DBG_ATTRIB_FLAGS attrib)
{
    Evaluation::DkmEvaluationResultStorageType::e resultStorage = Evaluation::DkmEvaluationResultStorageType::None;
    if ( attrib & DBG_ATTRIB_STORAGE_GLOBAL )
        resultStorage = Evaluation::DkmEvaluationResultStorageType::Global;
    else if ( attrib & DBG_ATTRIB_STORAGE_STATIC )
        resultStorage = Evaluation::DkmEvaluationResultStorageType::Static;
    else if ( attrib & DBG_ATTRIB_STORAGE_REGISTER )
        resultStorage = Evaluation::DkmEvaluationResultStorageType::Register;
    return resultStorage;
}

Evaluation::DkmEvaluationResultCategory::e toResultCategory(DBG_ATTRIB_FLAGS attrib)
{
    Evaluation::DkmEvaluationResultCategory::e cat = Evaluation::DkmEvaluationResultCategory::Other;
    if (attrib & DBG_ATTRIB_DATA)
        cat = Evaluation::DkmEvaluationResultCategory::Data;
    else if (attrib & DBG_ATTRIB_METHOD)
        cat = Evaluation::DkmEvaluationResultCategory::Method;
    else if (attrib & DBG_ATTRIB_PROPERTY)
        cat = Evaluation::DkmEvaluationResultCategory::Property;
    else if (attrib & DBG_ATTRIB_CLASS)
        cat = Evaluation::DkmEvaluationResultCategory::Class;
    else if (attrib & DBG_ATTRIB_BASECLASS)
        cat = Evaluation::DkmEvaluationResultCategory::BaseClass;
    else if (attrib & DBG_ATTRIB_INTERFACE)
        cat = Evaluation::DkmEvaluationResultCategory::Interface;
    else if (attrib & DBG_ATTRIB_INNERCLASS)
        cat = Evaluation::DkmEvaluationResultCategory::InnerClass;
    else if (attrib & DBG_ATTRIB_MOSTDERIVEDCLASS)
        cat = Evaluation::DkmEvaluationResultCategory::MostDerivedClass;
    return cat;
}

Evaluation::DkmEvaluationResultTypeModifierFlags::e toResultModifierFlags(DBG_ATTRIB_FLAGS attrib)
{
    Evaluation::DkmEvaluationResultTypeModifierFlags::e resultFlags = Evaluation::DkmEvaluationResultTypeModifierFlags::None;
    if ( attrib & DBG_ATTRIB_TYPE_VIRTUAL )
        resultFlags |= Evaluation::DkmEvaluationResultTypeModifierFlags::Virtual;
    if ( attrib & DBG_ATTRIB_TYPE_CONSTANT )
        resultFlags |= Evaluation::DkmEvaluationResultTypeModifierFlags::Constant;
    if ( attrib & DBG_ATTRIB_TYPE_SYNCHRONIZED )
        resultFlags |= Evaluation::DkmEvaluationResultTypeModifierFlags::Synchronized;
    if ( attrib & DBG_ATTRIB_TYPE_VOLATILE )
        resultFlags |= Evaluation::DkmEvaluationResultTypeModifierFlags::Volatile;
    return resultFlags;
}

HRESULT createEvaluationResult(Evaluation::DkmInspectionContext* pInspectionContext, CallStack::DkmStackWalkFrame* pStackFrame,
                               DEBUG_PROPERTY_INFO& info, Evaluation::DkmSuccessEvaluationResult** ppResultObject)
{
    HRESULT hr;
    CComPtr<Evaluation::DkmDataAddress> dataAddr;
    CComPtr<IDebugMemoryContext2> memctx;
    hr = info.pProperty->GetMemoryContext(&memctx);
    if(hr == S_OK) // doesn't fail for properties without context
    {
        CComPtr<Mago::IMagoMemoryContext> magoCtx;
        hr = memctx->QueryInterface(&magoCtx);
        if(SUCCEEDED(hr))
        {
            UINT64 addr = 0;
            tryHR(magoCtx->GetAddress(addr));
            tryHR(Evaluation::DkmDataAddress::Create(pInspectionContext->RuntimeInstance(), addr, nullptr, &dataAddr));
        }
    }

    Evaluation::DkmEvaluationResultFlags::e resultFlags = toResultFlags(info.dwAttrib);
    if (dataAddr)
        resultFlags |= Evaluation::DkmEvaluationResultFlags::Address;

    hr = Evaluation::DkmSuccessEvaluationResult::Create(
        pInspectionContext, pStackFrame, 
        toDkmString(info.bstrName), 
        toDkmString(info.bstrFullName),
        resultFlags,
        toDkmString(info.bstrValue), // display value
        toDkmString(info.bstrValue), // editable value
        toDkmString(info.bstrType),
        toResultCategory(info.dwAttrib), 
        toResultAccess(info.dwAttrib), 
        toResultStorage(info.dwAttrib),
        toResultModifierFlags(info.dwAttrib),
        dataAddr, // address
        nullptr, // UI visiualizers
        nullptr, // external modules
        DkmDataItem(info.pProperty, __uuidof(Mago::Property)),
        ppResultObject);
    return hr;
}

HRESULT createEvaluationError(Evaluation::DkmInspectionContext* pInspectionContext, CallStack::DkmStackWalkFrame* pStackFrame,
                              HRESULT hrErr, Evaluation::DkmLanguageExpression* expr,
                              IDkmCompletionRoutine<Evaluation::DkmEvaluateExpressionAsyncResult>* pCompletionRoutine)
{
    std::wstring errStr;
    Evaluation::DkmFailedEvaluationResult* pResultObject = nullptr;

    tryHR(MagoEE::GetErrorString(hrErr, errStr));
    tryHR(Evaluation::DkmFailedEvaluationResult::Create(
        pInspectionContext, pStackFrame, 
        expr->Text(), expr->Text(), toDkmString(errStr.c_str()), 
        Evaluation::DkmEvaluationResultFlags::None, nullptr, 
        DkmDataItem::Null(), &pResultObject));

    Evaluation::DkmEvaluateExpressionAsyncResult result;
    result.ErrorCode = hrErr;
    result.pResultObject = pResultObject;
    pCompletionRoutine->OnComplete(result);
    return S_OK;
}

///////////////////////////////////////////////////////////////////////////////
HRESULT STDMETHODCALLTYPE CMagoNatCCService::EvaluateExpression(
    _In_ Evaluation::DkmInspectionContext* pInspectionContext,
    _In_ DkmWorkList* pWorkList,
    _In_ Evaluation::DkmLanguageExpression* pExpression,
    _In_ CallStack::DkmStackWalkFrame* pStackFrame,
    _In_ IDkmCompletionRoutine<Evaluation::DkmEvaluateExpressionAsyncResult>* pCompletionRoutine)
{
    HRESULT hr;
    auto process = pInspectionContext->RuntimeInstance()->Process();

    RefPtr<CCExprContext> exprContext;
    tryHR(MakeCComObject(exprContext));
    tryHR(exprContext->Init(process, pStackFrame));

    std::wstring exprText = pExpression->Text()->Value();
    MagoEE::FormatOptions fmtopt;
    tryHR(MagoEE::StripFormatSpecifier(exprText, fmtopt));

    RefPtr<MagoEE::IEEDParsedExpr> pExpr;
    hr = MagoEE::ParseText(exprText.c_str(), exprContext->GetTypeEnv(), exprContext->GetStringTable(), pExpr.Ref());
    if (FAILED(hr))
        return createEvaluationError(pInspectionContext, pStackFrame, hr, pExpression, pCompletionRoutine);

    MagoEE::EvalOptions options = { 0 };
    hr = pExpr->Bind(options, exprContext);
    if (FAILED(hr))
        return createEvaluationError(pInspectionContext, pStackFrame, hr, pExpression, pCompletionRoutine);

    MagoEE::EvalResult value = { 0 };
    hr = pExpr->Evaluate(options, exprContext, value);
    if (FAILED(hr))
        return createEvaluationError(pInspectionContext, pStackFrame, hr, pExpression, pCompletionRoutine);

    RefPtr<Mago::Property> pProperty;
    tryHR(MakeCComObject(pProperty));
    tryHR(pProperty->Init(exprText.c_str(), exprText.c_str(), value, exprContext, fmtopt));

    int radix = pInspectionContext->Radix();
    int timeout = pInspectionContext->Timeout();
    DEBUG_PROPERTY_INFO info;
    tryHR(pProperty->GetPropertyInfo(DEBUGPROP_INFO_ALL, radix, timeout, nullptr, 0, &info));
    
    Evaluation::DkmSuccessEvaluationResult* pResultObject = nullptr;
    tryHR(createEvaluationResult(pInspectionContext, pStackFrame, info, &pResultObject));

    Evaluation::DkmEvaluateExpressionAsyncResult result;
    result.ErrorCode = S_OK;
    result.pResultObject = pResultObject;
    pCompletionRoutine->OnComplete(result);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CMagoNatCCService::GetChildren(
    _In_ Evaluation::DkmEvaluationResult* pResult,
    _In_ DkmWorkList* pWorkList,
    _In_ UINT32 InitialRequestSize,
    _In_ Evaluation::DkmInspectionContext* pInspectionContext,
    _In_ IDkmCompletionRoutine<Evaluation::DkmGetChildrenAsyncResult>* pCompletionRoutine)
{
    auto successResult = Evaluation::DkmSuccessEvaluationResult::TryCast(pResult);
    if (!successResult)
        return S_FALSE;

    RefPtr<Mago::Property> pProperty;
    tryHR(pResult->GetDataItem(&pProperty.Ref()));

    int radix = pInspectionContext->Radix();
    int timeout = pInspectionContext->Timeout();
    RefPtr<IEnumDebugPropertyInfo2> pEnum;
    tryHR(pProperty->EnumChildren(DEBUGPROP_INFO_ALL, radix, GUID(), DBG_ATTRIB_ALL, NULL, timeout, &pEnum.Ref()));
    ULONG count;
    tryHR(pEnum->GetCount(&count));

    CComPtr<Evaluation::DkmEvaluationResultEnumContext> pEnumContext;
    tryHR(Evaluation::DkmEvaluationResultEnumContext::Create(count, successResult->StackFrame(),
                                                             pInspectionContext, pEnum.Get(), &pEnumContext));

    Evaluation::DkmGetChildrenAsyncResult result;
    result.ErrorCode = S_OK;
    result.InitialChildren.Length = 0; // TODO
    result.InitialChildren.Members = nullptr;
    result.pEnumContext = pEnumContext;
    pCompletionRoutine->OnComplete(result);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CMagoNatCCService::GetFrameLocals(
    _In_ Evaluation::DkmInspectionContext* pInspectionContext,
    _In_ DkmWorkList* pWorkList,
    _In_ CallStack::DkmStackWalkFrame* pStackFrame,
    _In_ IDkmCompletionRoutine<Evaluation::DkmGetFrameLocalsAsyncResult>* pCompletionRoutine)
{
    RefPtr<CCExprContext> exprContext;
    tryHR(MakeCComObject(exprContext));
    auto process = pInspectionContext->RuntimeInstance()->Process();
    tryHR(exprContext->Init(process, pStackFrame));

    RefPtr<Mago::FrameProperty> frameProp;
    tryHR(MakeCComObject(frameProp));
    tryHR(frameProp->Init(exprContext->getRegSet(), exprContext));

    RefPtr<IEnumDebugPropertyInfo2> pEnum;
    tryHR(frameProp->EnumChildren(DEBUGPROP_INFO_ALL, pInspectionContext->Radix(),
                                  guidFilterLocalsPlusArgs, DBG_ATTRIB_ALL, NULL, pInspectionContext->Timeout(),
                                  &pEnum.Ref()));

    ULONG count;
    tryHR(pEnum->GetCount(&count));

    CComPtr<Evaluation::DkmEvaluationResultEnumContext> pEnumContext;
    tryHR(Evaluation::DkmEvaluationResultEnumContext::Create(count, pStackFrame,
                                                             pInspectionContext, pEnum.Get(), &pEnumContext));

    Evaluation::DkmGetFrameLocalsAsyncResult result;
    result.ErrorCode = S_OK;
    result.pEnumContext = pEnumContext;
    pCompletionRoutine->OnComplete(result);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CMagoNatCCService::GetFrameArguments(
    _In_ Evaluation::DkmInspectionContext* pInspectionContext,
    _In_ DkmWorkList* pWorkList,
    _In_ CallStack::DkmStackWalkFrame* pFrame,
    _In_ IDkmCompletionRoutine<Evaluation::DkmGetFrameArgumentsAsyncResult>* pCompletionRoutine)
{
//    return E_NOTIMPL;
    HRESULT hr = pInspectionContext->GetFrameArguments(pWorkList, pFrame, pCompletionRoutine);
    return hr;
}


HRESULT STDMETHODCALLTYPE CMagoNatCCService::GetItems(
    _In_ Evaluation::DkmEvaluationResultEnumContext* pEnumContext,
    _In_ DkmWorkList* pWorkList,
    _In_ UINT32 StartIndex,
    _In_ UINT32 Count,
    _In_ IDkmCompletionRoutine<Evaluation::DkmEvaluationEnumAsyncResult>* pCompletionRoutine)
{
    RefPtr<IEnumDebugPropertyInfo2> pEnum;
    tryHR(pEnumContext->GetDataItem(&pEnum.Ref()));

    ULONG children;
    tryHR(pEnum->GetCount(&children));

    if (StartIndex >= children)
        Count = 0;
    else
    {
        if (StartIndex + Count > children)
            Count = children - StartIndex;
        tryHR(pEnum->Reset());
        tryHR(pEnum->Skip(StartIndex));
    }
    Evaluation::DkmEvaluationEnumAsyncResult result;
    result.ErrorCode = S_OK;
    tryHR(DkmAllocArray(Count, &result.Items));
    for (ULONG i = 0; i < Count; i++)
    {
        ULONG fetched;
        DEBUG_PROPERTY_INFO info;
        Mago::_CopyPropertyInfo::init(&info);
        HRESULT hr = pEnum->Next(1, &info, &fetched);
        if (SUCCEEDED(hr) && fetched == 1 && info.pProperty)
        {
            Evaluation::DkmSuccessEvaluationResult* pResultObject = nullptr;
            hr = createEvaluationResult(pEnumContext->InspectionContext(), pEnumContext->StackFrame(), info, &pResultObject);
            if (SUCCEEDED(hr))
                result.Items.Members[i] = pResultObject;
            Mago::_CopyPropertyInfo::destroy(&info);
        }
    }
    pCompletionRoutine->OnComplete(result);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CMagoNatCCService::SetValueAsString(
    _In_ Evaluation::DkmEvaluationResult* pResult,
    _In_ DkmString* pValue,
    _In_ UINT32 Timeout,
    _Deref_out_opt_ DkmString** ppErrorText)
{
    RefPtr<Mago::Property> prop;
    tryHR(pResult->GetDataItem(&prop.Ref()));

    int radix = 10;
    tryHR(prop->SetValueAsString(pValue->Value(), radix, Timeout));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CMagoNatCCService::GetUnderlyingString(
    _In_ Evaluation::DkmEvaluationResult* pResult,
    _Deref_out_opt_ DkmString** ppStringValue)
{
    RefPtr<Mago::Property> prop;
    tryHR(pResult->GetDataItem(&prop.Ref()));

    ULONG len, fetched;
    tryHR(prop->GetStringCharLength(&len));
    std::unique_ptr<WCHAR> str (new WCHAR[len + 1]);
    tryHR(prop->GetStringChars(len + 1, str.get(), &fetched));
    if (fetched <= len)
        str.get()[fetched] = 0;
    *ppStringValue = toDkmString(str.get()).Detach();
    return S_OK;
}
