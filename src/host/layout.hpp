// Every fact about the game binary the host depends on, in one place. All of it was measured on this exact build
// (Ballest-Win64-Shipping.exe, UE 5.8, FileVersion ++UE5+Release-5.8-CL-56702186) with tools/memread.py against
// the running game, or from its PE headers; see docs/DESIGN.md section 2a. On this build the addresses below are used
// as they are. On any other build (a game update) the three tables are found again by what they contain (engine.cpp:
// the name pool by its "None" entry, the object array by objects that know their own slot, ProcessEvent by its vtable
// slot) and checked before use; the offsets inside engine objects belong to the engine version (UE 5.8) and stay.
//
// Naming: k<Type><What>Offset is the byte offset of a field inside an engine object of that type, and
// k<What>OffsetInExe is an address given relative to where the game exe is loaded. The Unreal source name of each
// field is in its comment, for searching the engine source.
#pragma once
#include <cstdint>

namespace layout {

// --- Build fingerprint (the exe's PE header) ---------------------------------------------------------------------
constexpr uint32_t kExpectedGameExeTimeDateStamp = 1949201014;   // IMAGE_FILE_HEADER::TimeDateStamp
constexpr uint32_t kExpectedGameExeSizeOfImage = 0xB008000;      // IMAGE_OPTIONAL_HEADER::SizeOfImage (bytes in memory)

// --- Addresses inside the game exe, relative to its load address ---------------------------------------------------
constexpr uintptr_t kGlobalObjectArrayOffsetInExe = 0xA39ED40;             // GUObjectArray: every live UObject
constexpr uintptr_t kNamePoolOffsetInExe = 0xA2D0D40;                      // FNamePool: every FName's text; found
                                                                           // through a rip-relative lea in FName::ToString
constexpr uintptr_t kProcessEventFunctionOffsetInExe = 0x15F22C0;          // UObject::ProcessEvent: calls a UFunction
constexpr uintptr_t kViewportClientTickFunctionOffsetInExe = 0x2A74F20;    // UGameViewportClient::Tick
constexpr int kViewportClientTickVtableSlot = 99;                          // its vtable slot (not overridden by
                                                                           // CommonGameViewportClient)
constexpr int kProcessEventVtableSlot = 74;                                // UObject::ProcessEvent in UObject's vtable
                                                                           // (measured on Default__Object, 2026-09-24)

// --- Global object array (FUObjectArray, whose ObjObjects is a FChunkedFixedUObjectArray at offset 0) -------------
constexpr int kObjectArrayChunkListOffset = 0x0;             // Objects: FUObjectItem** (one pointer per chunk)
constexpr int kObjectArrayObjectCountOffset = 0x8;           // NumElements: int32
constexpr int kObjectArrayItemsPerChunk = 65536;             // NumElementsPerChunk
constexpr int kObjectArrayItemSizeBytes = 24;                // sizeof(FUObjectItem)
constexpr int kObjectArrayItemObjectPointerOffset = 0x8;     // FUObjectItem::Object

// --- Name pool: block pointers, then per entry a u16 header (bit 0 = wide characters, length = header >> 6)
// followed by the characters. An entry lives at block + offset * 2. ----------------------------------------------
constexpr int kNamePoolBlockListOffset = 0x10;               // FNameEntryAllocator::Blocks

// --- UObject: the base of every engine object -------------------------------------------------------------------
constexpr int kUObjectFlagsOffset = 0x8;                     // ObjectFlags (measured; not used yet)
constexpr int kUObjectArrayIndexOffset = 0xC;                // InternalIndex: its slot in the global object array
constexpr int kUObjectClassOffset = 0x10;                    // ClassPrivate: UClass*
constexpr int kUObjectNameOffset = 0x18;                     // NamePrivate: FName
constexpr int kUObjectOuterOffset = 0x20;                    // OuterPrivate: the object that contains it

// --- UStruct (classes, structs, functions), UField, UFunction -----------------------------------------------------
constexpr int kUStructParentStructOffset = 0x40;             // SuperStruct: the class or struct it inherits from
constexpr int kUStructFirstFunctionOffset = 0x48;            // Children: UField* list (the functions)
constexpr int kUStructFirstPropertyOffset = 0x50;            // ChildProperties: FField* list (the properties)
constexpr int kUFieldNextFieldOffset = 0x28;                 // UField::Next
constexpr int kUFunctionParametersSizeOffset = 0xB6;         // ParmsSize: uint16, bytes of the parameter block
constexpr int kUFunctionNativeFunctionOffset = 0xD8;         // Func: what Invoke calls; one shared interpreter entry for
                                                             // every Blueprint function, its own thunk for a native one

// --- FFrame (the stack frame a UFunction's Func receives) --------------------------------------------------------
// Checked on every use: Node must be the function called and Object the object it was called on.
constexpr int kFFrameFunctionOffset = 0x10;                  // Node: UFunction*
constexpr int kFFrameObjectOffset = 0x18;                    // Object: UObject*
constexpr int kFFrameParametersOffset = 0x28;                // Locals: the parameter block

// --- FField (properties and their type descriptions) -------------------------------------------------------------
constexpr int kFFieldTypeOffset = 0x8;                       // ClassPrivate: FFieldClass*, the property's type
constexpr int kFFieldNextFieldOffset = 0x18;                 // Next
constexpr int kFFieldNameOffset = 0x20;                      // NamePrivate: FName
constexpr int kFFieldTypeNameOffset = 0x8;                   // FFieldClass::Name ("StructProperty", "BoolProperty", ...)

// --- FProperty and its subtypes ------------------------------------------------------------------------------------
constexpr int kFPropertyValueSizeOffset = 0x34;              // ElementSize: bytes of one value
constexpr int kFPropertyFlagsOffset = 0x38;                  // PropertyFlags: uint64 of the kPropertyFlag* bits below
constexpr int kFPropertyValueLocationOffset = 0x44;          // Offset_Internal: where the value sits in its owner
constexpr int kFStructPropertyStructTypeOffset = 0x70;       // FStructProperty::Struct: the UScriptStruct it holds
constexpr int kFBoolPropertyByteIndexOffset = 0x71;          // FBoolProperty::ByteOffset: which byte holds the bit
constexpr int kFBoolPropertyBitMaskOffset = 0x73;            // FBoolProperty::FieldMask: which bit of that byte
constexpr int kFPropertyArrayDimOffset = 0x30;               // ArrayDim: element count of a fixed array (1 otherwise)
// Fields of container properties, after FProperty's own 0x70 bytes. Checked by the type dump (eng::DumpTypes): every
// pointer read must lead to a property, and the dump matched the pre-update .usmap for types the update left alone.
constexpr int kFObjectPropertyClassOffset = 0x70;            // FObjectPropertyBase::PropertyClass
constexpr int kFBytePropertyEnumOffset = 0x70;               // FByteProperty::Enum (null for a plain byte)
constexpr int kFEnumPropertyUnderlyingOffset = 0x70;         // FEnumProperty::UnderlyingProp
constexpr int kFEnumPropertyEnumOffset = 0x78;               // FEnumProperty::Enum
constexpr int kFArrayPropertyInnerOffset = 0x78;             // FArrayProperty::Inner (after ArrayFlags)
constexpr int kFSetPropertyElementOffset = 0x70;             // FSetProperty::ElementProp
constexpr int kFMapPropertyKeyOffset = 0x70;                 // FMapProperty::KeyProp
constexpr int kFMapPropertyValueOffset = 0x78;               // FMapProperty::ValueProp
constexpr int kFOptionalPropertyValueOffset = 0x70;          // FOptionalProperty::ValueProperty
// UEnum on this engine (5.8, measured on ECollisionChannel, EMovementMode and two game enums): the entry names and
// values are two arrays, each behind a pointer whose lowest bit is set (masked off before use), with one count.
constexpr int kUEnumEntryNamesOffset = 0x40;                 // FName[count] (8 bytes each)
constexpr int kUEnumEntryValuesOffset = 0x48;                // int64[count]
constexpr int kUEnumEntryCountOffset = 0x50;                 // int32

// --- Property flag bits (EPropertyFlags) -------------------------------------------------------------------------
constexpr uint64_t kPropertyFlagIsParameter = 0x80;          // CPF_Parm
constexpr uint64_t kPropertyFlagIsOutParameter = 0x100;      // CPF_OutParm
constexpr uint64_t kPropertyFlagIsReturnValue = 0x400;       // CPF_ReturnParm

// --- Text: FText is 16 bytes { ITextData*, uint32 flags }; ITextData counts its own references
// (vtable at +0, uint32 count at +8). ---------------------------------------------------------------------------
constexpr int kTextDataReferenceCountOffset = 0x8;

}  // namespace layout
