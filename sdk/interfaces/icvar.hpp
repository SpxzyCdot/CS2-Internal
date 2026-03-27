#pragma once
#include <cstdint>
#include <string>

// Virtual Function Macro
#define VFUNC(index, returnType, ...) \
    return (*(returnType(__thiscall***)(void*, ##__VA_ARGS__))(*(uintptr_t*)this + index * 8))(this, ##__VA_ARGS__)

// Color for ConVar (simplistic)
struct Color
{
    unsigned char r, g, b, a;
};

// IConVar (abstract base for ConVars)
class IConVar
{
public:
    virtual void SetValue(const char* pValue) = 0;
    virtual void SetValue(float flValue) = 0;
    virtual void SetValue(int nValue) = 0;
    virtual void SetValue(Color value) = 0;
    virtual const char* GetName(void) const = 0;
    virtual const char* GetBaseName(void) const = 0;
    virtual bool IsFieldSet(int nFlags) const = 0;
    virtual int GetSplitScreenPlayerSlot() const = 0;
};

// CConVar (the actual ConVar object structure in CS2 is complex, we mostly need SetValue/GetValue)
// In CS2, ConVars are often wrapped. We'll use a simplified vtable approach to interact with them via the interface.

class ConVar
{
public:
    const char* szName;
    ConVar* pNext;
    bool bRegistered;
    const char* szHelpString;
    int nFlags;
    // ... more fields
    
    // VFunctions for setting values
    // Using a pattern or index is tricky without a full SDK. 
    // However, if we get the CConVar pointer from ICvar, the first few virtual functions are usually SetValue.
    // Index 16, 17, 18 etc. might be SetValue.
    // Actually, in CS2, it's safer to use the raw union/struct if we know it, BUT since we don't have the full struct,
    // we will rely on vfuncs if possible, OR just finding the float/string members offset.
    
    // For now, let's define the generic SetValue vfuncs commonly found:
    // 0: dtor
    // ...
    // Let's try to access the value directly if possible. 
    // CS2 ConVar struct:
    // Offset 0x40 = float value
    // Offset 0x44 = int value
    // Offset 0x48 = bool value?
    // This is risky.
    
    // A safer bet for CS2 internal cheats without full SDK is often just:
    // *(float*)((uintptr_t)pCvar + 0x40) = newValue;
    // *(int*)((uintptr_t)pCvar + 0x44) = newValue;
    
    // Helper helper
    void SetFloat(float val) {
        // vfunc 15, 16, 17? Or direct memory.
        // Direct memory is standard for simple internals.
        // Let's assume standard offsets for now.
        *(float*)((uintptr_t)this + 0x40) = val;
        *(int*)((uintptr_t)this + 0x44) = (int)val;
    }
    
    void SetInt(int val) {
        *(int*)((uintptr_t)this + 0x44) = val;
        *(float*)((uintptr_t)this + 0x40) = (float)val;
    }
    
    float GetFloat() {
        return *(float*)((uintptr_t)this + 0x40);
    }
    
    int GetInt() {
        return *(int*)((uintptr_t)this + 0x44);
    }
    
    const char* GetString() {
        return (const char*)((uintptr_t)this + 0x30); // Likely string value or ptr
    }
    
     void SetString(const char* val) {
         // This is harder via direct memory as it might need allocation.
         // Usually easier to find SetString vfunc.
         // Index 24 or 25?
         // For 'sv_skyname', we might just write to the buffer if it's large enough or replace pointer?
         // Actually, 'sv_skyname' changes might require the game to process it.
    }
};

// ICvar Interface
class ICvar
{
public:
    // FindVar is usually index 10 or 11 in CS2.
    // Let's assume index 10 based on common dumps.
    ConVar* FindVar(const char* name)
    {
        // 10: FindVar(const char*)
        typedef ConVar* (*FindVarFn)(void*, const char*);
        return (*(FindVarFn**)this)[10](this, name);
    }
    
    // Or iterating the list.
};
