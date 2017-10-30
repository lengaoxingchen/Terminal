/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "directio.h"

#include "_output.h"
#include "output.h"
#include "dbcs.h"
#include "handle.h"
#include "misc.h"
#include "../inc/convert.hpp"
#include "readDataDirect.hpp"

#include "ApiRoutines.h"

#include "..\interactivity\inc\ServiceLocator.hpp"

#pragma hdrstop

class CONSOLE_INFORMATION;

#define UNICODE_DBCS_PADDING 0xffff

// Routine Description:
// - converts non-unicode InputEvents to unicode InputEvents
// Arguments:
// inEvents - InputEvents to convert
// partialEvent - on output, will contain a partial dbcs byte char
// data if the last event in inEvents is a dbcs lead byte
// Return Value:
// - inEvents will contain unicode InputEvents
// - partialEvent may contain a partial dbcs KeyEvent
void EventsToUnicode(_Inout_ std::deque<std::unique_ptr<IInputEvent>>& inEvents,
                     _Outref_result_maybenull_ std::unique_ptr<IInputEvent>& partialEvent)
{
    const CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    std::deque<std::unique_ptr<IInputEvent>> outEvents;

    while (!inEvents.empty())
    {
        std::unique_ptr<IInputEvent> currentEvent = std::move(inEvents.front());
        inEvents.pop_front();

        if (currentEvent->EventType() != InputEventType::KeyEvent)
        {
            outEvents.push_back(std::move(currentEvent));
        }
        else
        {
            const KeyEvent* const keyEvent = static_cast<const KeyEvent* const>(currentEvent.get());

            wistd::unique_ptr<wchar_t[]> outWChar;
            size_t size;
            HRESULT hr;

            // convert char data to unicode
            if (IsDBCSLeadByteConsole(static_cast<char>(keyEvent->_charData), &gci->CPInfo))
            {
                if (inEvents.empty())
                {
                    // we ran out of data and have a partial byte leftover
                    partialEvent = std::move(currentEvent);
                    break;
                }

                // get the 2nd byte and convert to unicode
                const KeyEvent* const keyEventEndByte = static_cast<const KeyEvent* const>(inEvents.front().get());
                inEvents.pop_front();

                char inBytes[] =
                {
                    static_cast<char>(keyEvent->_charData),
                    static_cast<char>(keyEventEndByte->_charData)
                };
                hr = ConvertToW(gci->CP,
                                inBytes,
                                ARRAYSIZE(inBytes),
                                outWChar,
                                size);
            }
            else
            {
                char inBytes[] =
                {
                    static_cast<char>(keyEvent->_charData)
                };
                hr = ConvertToW(gci->CP,
                                inBytes,
                                ARRAYSIZE(inBytes),
                                outWChar,
                                size);
            }

            // push unicode key events back out
            if (SUCCEEDED(hr) && size > 0)
            {
                KeyEvent unicodeKeyEvent = *keyEvent;
                for (size_t i = 0; i < size; ++i)
                {
                    unicodeKeyEvent._charData = outWChar[i];
                    try
                    {
                        outEvents.push_back(std::make_unique<KeyEvent>(unicodeKeyEvent));
                    }
                    catch (...)
                    {
                        LOG_HR(wil::ResultFromCaughtException());
                    }
                }
            }
        }
    }

    inEvents.swap(outEvents);
    return;
}

// Routine Description:
// - This routine reads or peeks input events.  In both cases, the events
//   are copied to the user's buffer.  In the read case they are removed
//   from the input buffer and in the peek case they are not.
// Arguments:
// - pInputBuffer - The input buffer to take records from to return to the client
// - outEvents - The storage location to fill with input events
// - eventReadCount - The number of events to read
// - pInputReadHandleData - A structure that will help us maintain
// some input context across various calls on the same input
// handle. Primarily used to restore the "other piece" of partially
// returned strings (because client buffer wasn't big enough) on the
// next call.
// - IsUnicode - Whether to operate on Unicode characters or convert
// on the current Input Codepage.
// - IsPeek - If this is a peek operation (a.k.a. do not remove
// characters from the input buffer while copying to client buffer.)
// - ppWaiter - If we have to wait (not enough data to fill client
// buffer), this contains context that will allow the server to
// restore this call later.
// Return Value:
// - STATUS_SUCCESS - If data was found and ready for return to the client.
// - CONSOLE_STATUS_WAIT - If we didn't have enough data or needed to
// block, this will be returned along with context in *ppWaiter.
// - Or an out of memory/math/string error message in NTSTATUS format.
NTSTATUS DoGetConsoleInput(_In_ InputBuffer* const pInputBuffer,
                           _Out_ std::deque<std::unique_ptr<IInputEvent>>& outEvents,
                           _In_ const size_t eventReadCount,
                           _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                           _In_ bool const IsUnicode,
                           _In_ bool const IsPeek,
                           _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    *ppWaiter = nullptr;

    if (eventReadCount == 0)
    {
        return STATUS_SUCCESS;
    }

    LockConsole();
    auto Unlock = wil::ScopeExit([&] { UnlockConsole(); });

    std::deque<std::unique_ptr<IInputEvent>> partialEvents;
    if (!IsUnicode)
    {
        if (pInputBuffer->IsReadPartialByteSequenceAvailable())
        {
            partialEvents.push_back(pInputBuffer->FetchReadPartialByteSequence(IsPeek));
        }
    }

    size_t amountToRead;
    if (FAILED(SizeTSub(eventReadCount, partialEvents.size(), &amountToRead)))
    {
        return STATUS_INTEGER_OVERFLOW;
    }
    std::deque<std::unique_ptr<IInputEvent>> readEvents;
    NTSTATUS Status = pInputBuffer->Read(readEvents,
                                         amountToRead,
                                         IsPeek,
                                         true,
                                         IsUnicode);

    if (CONSOLE_STATUS_WAIT == Status)
    {
        assert(readEvents.empty());
        // If we're told to wait until later, move all of our context
        // to the read data object and send it back up to the server.
        try
        {
            *ppWaiter = new DirectReadData(pInputBuffer,
                                           pInputReadHandleData,
                                           eventReadCount,
                                           std::move(partialEvents));
        }
        catch (...)
        {
            *ppWaiter = nullptr;
            Status = STATUS_NO_MEMORY;
        }
    }
    else if (NT_SUCCESS(Status))
    {
        // split key events to oem chars if necessary
        if (!IsUnicode)
        {
            LOG_IF_FAILED(SplitToOem(readEvents));
        }

        // combine partial and readEvents
        while (!partialEvents.empty())
        {
            readEvents.push_front(std::move(partialEvents.back()));
            partialEvents.pop_back();
        }

        // move events over
        for (size_t i = 0; i < eventReadCount; ++i)
        {
            if (readEvents.empty())
            {
                break;
            }
            outEvents.push_back(std::move(readEvents.front()));
            readEvents.pop_front();
        }

        // store partial event if necessary
        if (!readEvents.empty())
        {
            pInputBuffer->StoreReadPartialByteSequence(std::move(readEvents.front()));
            readEvents.pop_front();
            assert(readEvents.empty());
        }
    }
    return Status;
}

// Routine Description:
// - Retrieves input records from the given input object and returns them to the client.
// - The peek version will NOT remove records when it copies them out.
// - The A version will convert to W using the console's current Input codepage (see SetConsoleCP)
// Arguments:
// - pInContext - The input buffer to take records from to return to the client
// - outEvents - storage location for read events
// - eventsToRead - The number of input events to read
// - pInputReadHandleData - A structure that will help us maintain
// some input context across various calls on the same input
// handle. Primarily used to restore the "other piece" of partially
// returned strings (because client buffer wasn't big enough) on the
// next call.
// - ppWaiter - If we have to wait (not enough data to fill client
// buffer), this contains context that will allow the server to
// restore this call later.
HRESULT ApiRoutines::PeekConsoleInputAImpl(_In_ IConsoleInputObject* const pInContext,
                                           _Out_ std::deque<std::unique_ptr<IInputEvent>>& outEvents,
                                           _In_ size_t const eventsToRead,
                                           _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                           _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    RETURN_NTSTATUS(DoGetConsoleInput(pInContext,
                                      outEvents,
                                      eventsToRead,
                                      pInputReadHandleData,
                                      false,
                                      true,
                                      ppWaiter));
}

// Routine Description:
// - Retrieves input records from the given input object and returns them to the client.
// - The peek version will NOT remove records when it copies them out.
// - The W version accepts UCS-2 formatted characters (wide characters)
// Arguments:
// - pInContext - The input buffer to take records from to return to the client
// - outEvents - storage location for read events
// - eventsToRead - The number of input events to read
// - pInputReadHandleData - A structure that will help us maintain
// some input context across various calls on the same input
// handle. Primarily used to restore the "other piece" of partially
// returned strings (because client buffer wasn't big enough) on the
// next call.
// - ppWaiter - If we have to wait (not enough data to fill client
// buffer), this contains context that will allow the server to
// restore this call later.
HRESULT ApiRoutines::PeekConsoleInputWImpl(_In_ IConsoleInputObject* const pInContext,
                                           _Out_ std::deque<std::unique_ptr<IInputEvent>>& outEvents,
                                           _In_ size_t const eventsToRead,
                                           _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                           _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    RETURN_NTSTATUS(DoGetConsoleInput(pInContext,
                                      outEvents,
                                      eventsToRead,
                                      pInputReadHandleData,
                                      true,
                                      true,
                                      ppWaiter));
}

// Routine Description:
// - Retrieves input records from the given input object and returns them to the client.
// - The read version WILL remove records when it copies them out.
// - The A version will convert to W using the console's current Input codepage (see SetConsoleCP)
// Arguments:
// - pInContext - The input buffer to take records from to return to the client
// - outEvents - storage location for read events
// - eventsToRead - The number of input events to read
// - pInputReadHandleData - A structure that will help us maintain
// some input context across various calls on the same input
// handle. Primarily used to restore the "other piece" of partially
// returned strings (because client buffer wasn't big enough) on the
// next call.
// - ppWaiter - If we have to wait (not enough data to fill client
// buffer), this contains context that will allow the server to
// restore this call later.
HRESULT ApiRoutines::ReadConsoleInputAImpl(_In_ IConsoleInputObject* const pInContext,
                                           _Out_ std::deque<std::unique_ptr<IInputEvent>>& outEvents,
                                           _In_ size_t const eventsToRead,
                                           _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                           _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    RETURN_NTSTATUS(DoGetConsoleInput(pInContext,
                                      outEvents,
                                      eventsToRead,
                                      pInputReadHandleData,
                                      false,
                                      false,
                                      ppWaiter));
}

// Routine Description:
// - Retrieves input records from the given input object and returns them to the client.
// - The read version WILL remove records when it copies them out.
// - The W version accepts UCS-2 formatted characters (wide characters)
// Arguments:
// - pInContext - The input buffer to take records from to return to the client
// - outEvents - storage location for read events
// - eventsToRead - The number of input events to read
// - pInputReadHandleData - A structure that will help us maintain
// some input context across various calls on the same input
// handle. Primarily used to restore the "other piece" of partially
// returned strings (because client buffer wasn't big enough) on the
// next call.
// - ppWaiter - If we have to wait (not enough data to fill client
// buffer), this contains context that will allow the server to
// restore this call later.
HRESULT ApiRoutines::ReadConsoleInputWImpl(_In_ IConsoleInputObject* const pInContext,
                                           _Out_ std::deque<std::unique_ptr<IInputEvent>>& outEvents,
                                           _In_ size_t const eventsToRead,
                                           _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                           _Outptr_result_maybenull_ IWaitRoutine** const ppWaiter)
{
    RETURN_NTSTATUS(DoGetConsoleInput(pInContext,
                                      outEvents,
                                      eventsToRead,
                                      pInputReadHandleData,
                                      true,
                                      false,
                                      ppWaiter));
}

// Routine Description:
// - Writes events to the input buffer
// Arguments:
// - pInputBuffer - the input buffer to write to
// - events - the events to written
// - eventsWritten - on output, the number of events written
// - unicode - true if events are unicode char data
// - append - true if events should be written to the end of the input
// buffer, false if they should be written to the front
// Return Value:
// - HRESULT indicating success or failure
HRESULT DoSrvWriteConsoleInput(_Inout_ InputBuffer* const pInputBuffer,
                               _Inout_ std::deque<std::unique_ptr<IInputEvent>>& events,
                               _Out_ size_t& eventsWritten,
                               _In_ const bool unicode,
                               _In_ const bool append)
{
    LockConsole();
    auto Unlock = wil::ScopeExit([&] { UnlockConsole(); });

    eventsWritten = 0;

    try
    {
        // add partial byte event if necessary
        if (pInputBuffer->IsWritePartialByteSequenceAvailable())
        {
            events.push_front(pInputBuffer->FetchWritePartialByteSequence(false));
        }

        // convert to unicode if necessary
        if (!unicode)
        {
            std::unique_ptr<IInputEvent> partialEvent;
            EventsToUnicode(events, partialEvent);

            if (partialEvent.get())
            {
                pInputBuffer->StoreWritePartialByteSequence(std::move(partialEvent));
            }
        }
    }
    CATCH_RETURN();

    // add to InputBuffer
    if (append)
    {
        eventsWritten = static_cast<ULONG>(pInputBuffer->Write(events));
    }
    else
    {
        eventsWritten = static_cast<ULONG>(pInputBuffer->Prepend(events));
    }

    return S_OK;
}

// Function Description:
// - Writes the input records to the beginning of the input buffer. This is used
//      by VT sequences that need a response immediately written back to the 
//      input.
// Arguments:
// - pInputBuffer - the input buffer to write to
// - events - the events to written
// - eventsWritten - on output, the number of events written
// Return Value:
// - HRESULT indicating success or failure
HRESULT DoSrvPrivatePrependConsoleInput(_Inout_ InputBuffer* const pInputBuffer,
                                        _Inout_ std::deque<std::unique_ptr<IInputEvent>>& events,
                                        _Out_ size_t& eventsWritten)
{
    LockConsole();
    auto Unlock = wil::ScopeExit([&] { UnlockConsole(); });

    eventsWritten = 0;

    try
    {
        // add partial byte event if necessary
        if (pInputBuffer->IsWritePartialByteSequenceAvailable())
        {
            events.push_front(pInputBuffer->FetchWritePartialByteSequence(false));
        }
    }
    CATCH_RETURN();

    // add to InputBuffer
    eventsWritten = pInputBuffer->Prepend(events);

    return S_OK;
}

// Routine Description:
// - This is used when the app reads oem from the output buffer the app wants
//   real OEM characters. We have real Unicode or UnicodeOem.
NTSTATUS TranslateOutputToOem(_Inout_ PCHAR_INFO OutputBuffer, _In_ COORD Size)
{
    const CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    ULONG NumBytes;
    ULONG uX, uY;
    if (FAILED(ShortToULong(Size.X, &uX)) ||
        FAILED(ShortToULong(Size.Y, &uY)) ||
        FAILED(ULongMult(uX, uY, &NumBytes)) ||
        FAILED(ULongMult(NumBytes, 2 * sizeof(CHAR_INFO), &NumBytes)))
    {
        return STATUS_NO_MEMORY;
    }
    PCHAR_INFO TmpBuffer = (PCHAR_INFO) new BYTE[NumBytes];
    PCHAR_INFO const SaveBuffer = TmpBuffer;
    if (TmpBuffer == nullptr)
    {
        return STATUS_NO_MEMORY;
    }

    UINT const Codepage = gci->OutputCP;

    memmove(TmpBuffer, OutputBuffer, Size.X * Size.Y * sizeof(CHAR_INFO));

    #pragma prefast(push)
    #pragma prefast(disable:26019, "The buffer is the correct size for any given DBCS characters. No additional validation needed.")
    for (int i = 0; i < Size.Y; i++)
    {
        for (int j = 0; j < Size.X; j++)
        {
            if (IsFlagSet(TmpBuffer->Attributes, COMMON_LVB_LEADING_BYTE))
            {
                if (j < Size.X - 1)
                {   // -1 is safe DBCS in buffer
                    j++;
                    CHAR AsciiDbcs[2] = { 0 };
                    NumBytes = sizeof(AsciiDbcs);
                    NumBytes = ConvertToOem(Codepage, &TmpBuffer->Char.UnicodeChar, 1, &AsciiDbcs[0], NumBytes);
                    OutputBuffer->Char.AsciiChar = AsciiDbcs[0];
                    OutputBuffer->Attributes = TmpBuffer->Attributes;
                    OutputBuffer++;
                    TmpBuffer++;
                    OutputBuffer->Char.AsciiChar = AsciiDbcs[1];
                    OutputBuffer->Attributes = TmpBuffer->Attributes;
                    OutputBuffer++;
                    TmpBuffer++;
                }
                else
                {
                    OutputBuffer->Char.AsciiChar = ' ';
                    OutputBuffer->Attributes = TmpBuffer->Attributes;
                    ClearAllFlags(OutputBuffer->Attributes, COMMON_LVB_SBCSDBCS);
                    OutputBuffer++;
                    TmpBuffer++;
                }
            }
            else if (AreAllFlagsClear(TmpBuffer->Attributes, COMMON_LVB_SBCSDBCS))
            {
                ConvertToOem(Codepage, &TmpBuffer->Char.UnicodeChar, 1, &OutputBuffer->Char.AsciiChar, 1);
                OutputBuffer->Attributes = TmpBuffer->Attributes;
                OutputBuffer++;
                TmpBuffer++;
            }
        }
    }
#pragma prefast(pop)

    delete[] SaveBuffer;
    return STATUS_SUCCESS;
}

// Routine Description:
// - This is used when the app writes oem to the output buffer we want
//   UnicodeOem or Unicode in the buffer, depending on font
NTSTATUS TranslateOutputToUnicode(_Inout_ PCHAR_INFO OutputBuffer, _In_ COORD Size)
{
    const CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();
    UINT const Codepage = gci->OutputCP;

    for (int i = 0; i < Size.Y; i++)
    {
        for (int j = 0; j < Size.X; j++)
        {
            ClearAllFlags(OutputBuffer->Attributes, COMMON_LVB_SBCSDBCS);
            if (IsDBCSLeadByteConsole(OutputBuffer->Char.AsciiChar, &gci->OutputCPInfo))
            {
                if (j < Size.X - 1)
                {   // -1 is safe DBCS in buffer
                    j++;
                    CHAR AsciiDbcs[2];
                    AsciiDbcs[0] = OutputBuffer->Char.AsciiChar;
                    AsciiDbcs[1] = (OutputBuffer + 1)->Char.AsciiChar;

                    WCHAR UnicodeDbcs[2];
                    ConvertOutputToUnicode(Codepage, &AsciiDbcs[0], 2, &UnicodeDbcs[0], 2);

                    OutputBuffer->Char.UnicodeChar = UnicodeDbcs[0];
                    SetFlag(OutputBuffer->Attributes, COMMON_LVB_LEADING_BYTE);
                    OutputBuffer++;
                    OutputBuffer->Char.UnicodeChar = UNICODE_DBCS_PADDING;
                    ClearAllFlags(OutputBuffer->Attributes, COMMON_LVB_SBCSDBCS);
                    SetFlag(OutputBuffer->Attributes, COMMON_LVB_TRAILING_BYTE);
                    OutputBuffer++;
                }
                else
                {
                    OutputBuffer->Char.UnicodeChar = UNICODE_SPACE;
                    OutputBuffer++;
                }
            }
            else
            {
                CHAR c = OutputBuffer->Char.AsciiChar;

                ConvertOutputToUnicode(Codepage, &c, 1, &OutputBuffer->Char.UnicodeChar, 1);
                OutputBuffer++;
            }
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS TranslateOutputToPaddingUnicode(_Inout_ PCHAR_INFO OutputBuffer, _In_ COORD Size, _Inout_ PCHAR_INFO OutputBufferR)
{
    DBGCHARS(("TranslateOutputToPaddingUnicode\n"));

    for (SHORT i = 0; i < Size.Y; i++)
    {
        for (SHORT j = 0; j < Size.X; j++)
        {
            WCHAR wch = OutputBuffer->Char.UnicodeChar;

            if (OutputBufferR)
            {
                OutputBufferR->Attributes = OutputBuffer->Attributes;
                ClearAllFlags(OutputBufferR->Attributes, COMMON_LVB_SBCSDBCS);
                if (IsCharFullWidth(OutputBuffer->Char.UnicodeChar))
                {
                    if (j == Size.X - 1)
                    {
                        OutputBufferR->Char.UnicodeChar = UNICODE_SPACE;
                    }
                    else
                    {
                        OutputBufferR->Char.UnicodeChar = OutputBuffer->Char.UnicodeChar;
                        SetFlag(OutputBufferR->Attributes, COMMON_LVB_LEADING_BYTE);
                        OutputBufferR++;
                        OutputBufferR->Char.UnicodeChar = UNICODE_DBCS_PADDING;
                        OutputBufferR->Attributes = OutputBuffer->Attributes;
                        ClearAllFlags(OutputBufferR->Attributes, COMMON_LVB_SBCSDBCS);
                        SetFlag(OutputBufferR->Attributes, COMMON_LVB_TRAILING_BYTE);
                    }
                }
                else
                {
                    OutputBufferR->Char.UnicodeChar = OutputBuffer->Char.UnicodeChar;
                }
                OutputBufferR++;
            }

            if (IsCharFullWidth(wch))
            {
                if (j != Size.X - 1)
                {
                    j++;
                }
            }
            OutputBuffer++;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS SrvReadConsoleOutput(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_READCONSOLEOUTPUT_MSG const a = &m->u.consoleMsgL2.ReadConsoleOutput;

    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::ReadConsoleOutput, a->Unicode);

    PCHAR_INFO Buffer;
    ULONG Size;
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(m->GetOutputBuffer((PVOID*)&Buffer, &Size));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleHandleData* HandleData = m->GetObjectHandle();
    if (HandleData == nullptr)
    {
        Status = STATUS_INVALID_HANDLE;
    }

    SCREEN_INFORMATION* pScreenInfo = nullptr;
    if (NT_SUCCESS(Status))
    {
        Status = NTSTATUS_FROM_HRESULT(HandleData->GetScreenBuffer(GENERIC_READ, &pScreenInfo));
    }

    if (!NT_SUCCESS(Status))
    {
        // a region of zero size is indicated by the right and bottom coordinates being less than the left and top.
        a->CharRegion.Right = (USHORT)(a->CharRegion.Left - 1);
        a->CharRegion.Bottom = (USHORT)(a->CharRegion.Top - 1);
    }
    else
    {
        COORD BufferSize;
        BufferSize.X = (SHORT)(a->CharRegion.Right - a->CharRegion.Left + 1);
        BufferSize.Y = (SHORT)(a->CharRegion.Bottom - a->CharRegion.Top + 1);

        if (((BufferSize.X > 0) && (BufferSize.Y > 0)) &&
            ((BufferSize.X * BufferSize.Y > ULONG_MAX / sizeof(CHAR_INFO)) || (Size < BufferSize.X * BufferSize.Y * sizeof(CHAR_INFO))))
        {
            UnlockConsole();
            return STATUS_INVALID_PARAMETER;
        }

        SCREEN_INFORMATION* const psi = pScreenInfo->GetActiveBuffer();

        Status = ReadScreenBuffer(psi, Buffer, &a->CharRegion);
        if (!a->Unicode)
        {
            TranslateOutputToOem(Buffer, BufferSize);
        }
        else if (!psi->TextInfo->GetCurrentFont()->IsTrueTypeFont())
        {
            // For compatibility reasons, we must maintain the behavior that munges the data if we are writing while a raster font is enabled.
            // This can be removed when raster font support is removed.
            RemoveDbcsMarkCell(Buffer, Buffer, BufferSize.X * BufferSize.Y);
        }

        if (NT_SUCCESS(Status))
        {
            m->SetReplyInformation(CalcWindowSizeX(&a->CharRegion) * CalcWindowSizeY(&a->CharRegion) * sizeof(CHAR_INFO));
        }
    }

    UnlockConsole();
    return Status;
}

NTSTATUS SrvWriteConsoleOutput(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_WRITECONSOLEOUTPUT_MSG const a = &m->u.consoleMsgL2.WriteConsoleOutput;

    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::WriteConsoleOutput, a->Unicode);

    PCHAR_INFO Buffer;
    ULONG Size;
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(m->GetInputBuffer((PVOID*)&Buffer, &Size));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleHandleData* HandleData = m->GetObjectHandle();
    if (HandleData == nullptr)
    {
        Status = STATUS_INVALID_HANDLE;
    }

    SCREEN_INFORMATION* pScreenInfo = nullptr;
    if (NT_SUCCESS(Status))
    {
        Status = NTSTATUS_FROM_HRESULT(HandleData->GetScreenBuffer(GENERIC_WRITE, &pScreenInfo));
    }
    if (!NT_SUCCESS(Status))
    {
        // A region of zero size is indicated by the right and bottom coordinates being less than the left and top.
        a->CharRegion.Right = (USHORT)(a->CharRegion.Left - 1);
        a->CharRegion.Bottom = (USHORT)(a->CharRegion.Top - 1);
    }
    else
    {
        COORD BufferSize;
        ULONG NumBytes;

        BufferSize.X = (SHORT)(a->CharRegion.Right - a->CharRegion.Left + 1);
        BufferSize.Y = (SHORT)(a->CharRegion.Bottom - a->CharRegion.Top + 1);
        if (FAILED(ULongMult(BufferSize.X, BufferSize.Y, &NumBytes)) ||
            FAILED(ULongMult(NumBytes, sizeof(*Buffer), &NumBytes)) ||
            (Size < NumBytes))
        {

            UnlockConsole();
            return STATUS_INVALID_PARAMETER;
        }

        PSCREEN_INFORMATION const ScreenBufferInformation = pScreenInfo->GetActiveBuffer();

        if (!a->Unicode)
        {
            TranslateOutputToUnicode(Buffer, BufferSize);
            Status = WriteScreenBuffer(ScreenBufferInformation, Buffer, &a->CharRegion);
        }
        else if (!ScreenBufferInformation->TextInfo->GetCurrentFont()->IsTrueTypeFont())
        {
            // For compatibility reasons, we must maintain the behavior that munges the data if we are writing while a raster font is enabled.
            // This can be removed when raster font support is removed.

            PCHAR_INFO TransBuffer = nullptr;
            int cBufferResult;
            if (SUCCEEDED(Int32Mult(BufferSize.X, BufferSize.Y, &cBufferResult)))
            {
                if (SUCCEEDED(Int32Mult(cBufferResult, 2 * sizeof(CHAR_INFO), &cBufferResult)))
                {
                    TransBuffer = (PCHAR_INFO)new BYTE[cBufferResult];
                }
            }

            if (TransBuffer == nullptr)
            {
                UnlockConsole();
                return STATUS_NO_MEMORY;
            }

            TranslateOutputToPaddingUnicode(Buffer, BufferSize, &TransBuffer[0]);
            Status = WriteScreenBuffer(ScreenBufferInformation, &TransBuffer[0], &a->CharRegion);
            delete[] TransBuffer;
        }
        else
        {
            Status = WriteScreenBuffer(ScreenBufferInformation, Buffer, &a->CharRegion);
        }

        if (NT_SUCCESS(Status))
        {
            // cause screen to be updated
            WriteToScreen(ScreenBufferInformation, a->CharRegion);
        }
    }

    UnlockConsole();
    return Status;
}


NTSTATUS SrvReadConsoleOutputString(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_READCONSOLEOUTPUTSTRING_MSG const a = &m->u.consoleMsgL2.ReadConsoleOutputString;

    switch (a->StringType)
    {
    case CONSOLE_ATTRIBUTE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::ReadConsoleOutputAttribute);
        break;
    case CONSOLE_ASCII:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::ReadConsoleOutputCharacter, false);
        break;
    case CONSOLE_REAL_UNICODE:
    case CONSOLE_FALSE_UNICODE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::ReadConsoleOutputCharacter, true);
        break;
    }

    PVOID Buffer;
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(m->GetOutputBuffer(&Buffer, &a->NumRecords));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleHandleData* HandleData = m->GetObjectHandle();
    if (HandleData == nullptr)
    {
        Status = NTSTATUS_FROM_WIN32(ERROR_INVALID_HANDLE);
    }

    SCREEN_INFORMATION* pScreenInfo = nullptr;
    if (NT_SUCCESS(Status))
    {
        Status = NTSTATUS_FROM_HRESULT(HandleData->GetScreenBuffer(GENERIC_READ, &pScreenInfo));
    }
    if (!NT_SUCCESS(Status))
    {
        // a region of zero size is indicated by the right and bottom coordinates being less than the left and top.
        a->NumRecords = 0;
    }
    else
    {
        ULONG nSize;

        if (a->StringType == CONSOLE_ASCII)
        {
            nSize = sizeof(CHAR);
        }
        else
        {
            nSize = sizeof(WORD);
        }

        a->NumRecords /= nSize;

        Status = ReadOutputString(pScreenInfo->GetActiveBuffer(), Buffer, a->ReadCoord, a->StringType, &a->NumRecords);

        if (NT_SUCCESS(Status))
        {
            m->SetReplyInformation(a->NumRecords * nSize);
        }
    }

    UnlockConsole();
    return Status;
}

NTSTATUS SrvWriteConsoleOutputString(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_WRITECONSOLEOUTPUTSTRING_MSG const a = &m->u.consoleMsgL2.WriteConsoleOutputString;

    auto tracing = wil::ScopeExit([&]()
    {
        Tracing::s_TraceApi(a);
    });

    switch (a->StringType)
    {
    case CONSOLE_ATTRIBUTE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::WriteConsoleOutputAttribute);
        break;
    case CONSOLE_ASCII:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::WriteConsoleOutputCharacter, false);
        break;
    case CONSOLE_REAL_UNICODE:
    case CONSOLE_FALSE_UNICODE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::WriteConsoleOutputCharacter, true);
        break;
    }

    PVOID Buffer;
    ULONG BufferSize;
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(m->GetInputBuffer(&Buffer, &BufferSize));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    CONSOLE_INFORMATION *Console;
    Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleHandleData* HandleData = m->GetObjectHandle();
    if (HandleData == nullptr)
    {
        Status = NTSTATUS_FROM_WIN32(ERROR_INVALID_HANDLE);
    }

    SCREEN_INFORMATION* pScreenInfo = nullptr;
    if (NT_SUCCESS(Status))
    {
        Status = NTSTATUS_FROM_HRESULT(HandleData->GetScreenBuffer(GENERIC_WRITE, &pScreenInfo));
    }
    if (!NT_SUCCESS(Status))
    {
        a->NumRecords = 0;
    }
    else
    {
        if (a->WriteCoord.X < 0 || a->WriteCoord.Y < 0)
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            if (a->StringType == CONSOLE_ASCII)
            {
                a->NumRecords = BufferSize;
            }
            else
            {
                a->NumRecords = BufferSize / sizeof(WCHAR);
            }

            Status = WriteOutputString(pScreenInfo->GetActiveBuffer(), Buffer, a->WriteCoord, a->StringType, &a->NumRecords, nullptr);
        }
    }

    UnlockConsole();
    return Status;
}

NTSTATUS SrvFillConsoleOutput(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_FILLCONSOLEOUTPUT_MSG const a = &m->u.consoleMsgL2.FillConsoleOutput;

    switch (a->ElementType)
    {
    case CONSOLE_ATTRIBUTE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::FillConsoleOutputAttribute);
        break;
    case CONSOLE_ASCII:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::FillConsoleOutputCharacter, false);
        break;
    case CONSOLE_REAL_UNICODE:
    case CONSOLE_FALSE_UNICODE:
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::FillConsoleOutputCharacter, true);
        break;
    }

    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    ConsoleHandleData* HandleData = m->GetObjectHandle();
    if (HandleData == nullptr)
    {
        Status = NTSTATUS_FROM_WIN32(ERROR_INVALID_HANDLE);
    }

    SCREEN_INFORMATION* pScreenInfo = nullptr;
    if (NT_SUCCESS(Status))
    {
        Status = NTSTATUS_FROM_HRESULT(HandleData->GetScreenBuffer(GENERIC_WRITE, &pScreenInfo));
    }
    if (!NT_SUCCESS(Status))
    {
        a->Length = 0;
    }
    else
    {
        Status = DoSrvFillConsoleOutput(pScreenInfo->GetActiveBuffer(), a);
    }

    UnlockConsole();
    return Status;
}

NTSTATUS DoSrvFillConsoleOutput(_In_ SCREEN_INFORMATION* pScreenInfo, _Inout_ CONSOLE_FILLCONSOLEOUTPUT_MSG* pMsg)
{
    return FillOutput(pScreenInfo, pMsg->Element, pMsg->WriteCoord, pMsg->ElementType, &pMsg->Length);
}

// There used to be a text mode and a graphics mode flag.
// Text mode was used for regular applications like CMD.exe.
// Graphics mode was used for bitmap VDM buffers and is no longer supported.
// OEM console font mode used to represent rewriting the entire buffer into codepage 437 so the renderer could handle it with raster fonts.
//  But now the entire buffer is always kept in Unicode and the renderer asks for translation when/if necessary for raster fonts only.
// We keep these definitions here so the API can enforce that the only one we support any longer is the original text mode.
// See: https://msdn.microsoft.com/en-us/library/windows/desktop/ms682122(v=vs.85).aspx
#define CONSOLE_TEXTMODE_BUFFER 1
//#define CONSOLE_GRAPHICS_BUFFER 2
//#define CONSOLE_OEMFONT_DISPLAY 4

NTSTATUS ConsoleCreateScreenBuffer(_Out_ ConsoleHandleData** ppHandle,
                                   _In_ PCONSOLE_API_MSG /*Message*/,
                                   _In_ PCD_CREATE_OBJECT_INFORMATION Information,
                                   _In_ PCONSOLE_CREATESCREENBUFFER_MSG a)
{
    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::CreateConsoleScreenBuffer);
    const CONSOLE_INFORMATION* const gci = ServiceLocator::LocateGlobals()->getConsoleInformation();

    // If any buffer type except the one we support is set, it's invalid.
    if (IsAnyFlagSet(a->Flags, ~CONSOLE_TEXTMODE_BUFFER))
    {
        ASSERT(false); // We no longer support anything other than a textmode buffer
        return STATUS_INVALID_PARAMETER;
    }

    ConsoleHandleData::HandleType const HandleType = ConsoleHandleData::HandleType::Output;

    const SCREEN_INFORMATION* const psiExisting = gci->CurrentScreenBuffer;

    // Create new screen buffer.
    CHAR_INFO Fill;
    Fill.Char.UnicodeChar = UNICODE_SPACE;
    Fill.Attributes = psiExisting->GetAttributes().GetLegacyAttributes();

    COORD WindowSize;
    WindowSize.X = (SHORT)psiExisting->GetScreenWindowSizeX();
    WindowSize.Y = (SHORT)psiExisting->GetScreenWindowSizeY();

    const FontInfo* const pfiExistingFont = psiExisting->TextInfo->GetCurrentFont();

    PSCREEN_INFORMATION ScreenInfo = nullptr;
    NTSTATUS Status = SCREEN_INFORMATION::CreateInstance(WindowSize,
                                                         pfiExistingFont,
                                                         WindowSize,
                                                         Fill,
                                                         Fill,
                                                         CURSOR_SMALL_SIZE,
                                                         &ScreenInfo);

    if (!NT_SUCCESS(Status))
    {
        goto Exit;
    }

    Status = NTSTATUS_FROM_HRESULT(ScreenInfo->Header.AllocateIoHandle(HandleType,
                                                                       Information->DesiredAccess,
                                                                       Information->ShareMode,
                                                                       ppHandle));

    if (!NT_SUCCESS(Status))
    {
        goto Exit;
    }

    SCREEN_INFORMATION::s_InsertScreenBuffer(ScreenInfo);

Exit:
    if (!NT_SUCCESS(Status))
    {
        delete ScreenInfo;
    }

    return Status;
}
