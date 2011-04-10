/////////////////////////////////////////////////////////////////////////////
// Name:            direction.cpp
// Purpose:         Stores and renders directions
// Author:          Brad Larsen
// Modified by:     
// Created:         Jan 12, 2005
// RCS-ID:          
// Copyright:       (c) Brad Larsen
// License:         wxWindows license
/////////////////////////////////////////////////////////////////////////////

#include "direction.h"

#include "powertabinputstream.h"
#include "powertaboutputstream.h"

// Default constants
const uint8_t        Direction::DEFAULT_POSITION                 = 0;

// Position Constants
const uint32_t      Direction::MIN_POSITION                     = 0;
const uint32_t      Direction::MAX_POSITION                     = 255;

// Symbol Constants
const uint8_t        Direction::MAX_SYMBOLS                      = 3;
const uint8_t        Direction::NUM_SYMBOL_TYPES                 = 19;

// Repeat Number Constants
const uint8_t        Direction::MIN_REPEAT_NUMBER                = 0;
const uint8_t        Direction::MAX_REPEAT_NUMBER                = 24;

static std::string directionText[Direction::NUM_SYMBOL_TYPES] =
{
    "Coda", "Double Coda", "Segno", "Segno Segno",
    "Fine", "D.C.", "D.S.", "D.S.S.", "To Coda",
    "To Dbl. Coda", "D.C. al Coda", "D.C. al Dbl. Coda",
    "D.S. al Coda", "D.S. al Dbl. Coda", "D.S.S. al Coda",
    "D.S.S. al Dbl. Coda", "D.C. al Fine", "D.S. al Fine",
    "D.S.S. al Fine"
};

// Constructor/Destructor
/// Default Constructor
Direction::Direction() : 
    m_position(DEFAULT_POSITION)
{
    //------Last Checked------//
    // - Jan 11, 2005
}

/// Primary Constructor
/// @param position Zero-based index of the position within the system where the
/// barline is anchored
/// @param symbolType Type of symbol to add (see symbolTypes enum for values)
/// @param activeSymbol Symbol that must be active for the symbol to be
/// triggered (see activeSymbols enum for values)
/// @param repeatNumber Repeat number that must be active for the symbol to be
/// triggered (0 = none)
Direction::Direction(uint32_t position, uint8_t symbolType, uint8_t activeSymbol,
    uint8_t repeatNumber) : m_position(position)
{
    //------Last Checked------//
    // - Jan 11, 2005
    assert(IsValidPosition(position));
    AddSymbol(symbolType, activeSymbol, repeatNumber);   
}

/// Copy constructor
Direction::Direction(const Direction& direction) :
    m_position(DEFAULT_POSITION)
{
    //------Last Checked------//
    // - Jan 11, 2005
    *this = direction;
}

/// Destructor
Direction::~Direction()
{
    //------Last Checked------//
    // - Jan 11, 2005
    DeleteSymbolArrayContents();
}

/// Assignment Operator
const Direction& Direction::operator=(const Direction& direction)
{
    //------Last Checked------//
    // - Jan 11, 2005

    // Check for assignment to self
    if (this != &direction)
    {
        m_position = direction.m_position;
    
        DeleteSymbolArrayContents();
        
        size_t i = 0;
        size_t count = direction.m_symbolArray.size();
        for (; i < count; i++)
            m_symbolArray.push_back(direction.m_symbolArray[i]);
    }
    return (*this);
}

/// Equality Operator
bool Direction::operator==(const Direction& direction) const
{
    //------Last Checked------//
    // - Jan 11, 2005
    
    size_t thisSymbolCount = GetSymbolCount();
    size_t otherSymbolCount = direction.GetSymbolCount();
    
    // Directions have differing number of symbols
    if (thisSymbolCount != otherSymbolCount)
        return (false);

    // All symbols must match
    size_t i = 0;
    for (; i < thisSymbolCount; i++)
    {
        if (m_symbolArray[i] != direction.m_symbolArray[i])
            return (false);
    }

    return (m_position == direction.m_position);
}

/// Inequality Operator
bool Direction::operator!=(const Direction& direction) const
{
    //------Last Checked------//
    // - Jan 5, 2005
    return (!operator==(direction));
}

// Serialize Functions
/// Performs serialization for the class
/// @param stream Power Tab output stream to serialize to
/// @return True if the object was serialized, false if not
bool Direction::Serialize(PowerTabOutputStream& stream)
{
    //------Last Checked------//
    // - Jan 11, 2005
    stream << m_position;
    CHECK_THAT(stream.CheckState(), false);
           
    size_t symbolCount = GetSymbolCount();
    stream << (uint8_t)symbolCount;
    CHECK_THAT(stream.CheckState(), false);

    size_t i = 0;
    for (; i < symbolCount; i++)
    {
        stream << m_symbolArray[i];
        CHECK_THAT(stream.CheckState(), false);
    }

    return (stream.CheckState());
}

/// Performs deserialization for the class
/// @param stream Power Tab input stream to load from
/// @param version File version
/// @return True if the object was deserialized, false if not
bool Direction::Deserialize(PowerTabInputStream& stream, uint16_t version)
{
    UNUSED(version);

    stream >> m_position;
    CHECK_THAT(stream.CheckState(), false);
    
    uint8_t symbolCount;
    stream >> symbolCount;
    CHECK_THAT(stream.CheckState(), false);

    size_t i = 0;
    for (i = 0; i < symbolCount; i++)
    {
        uint16_t symbol = 0;
        stream >> symbol;
        CHECK_THAT(stream.CheckState(), false);
        
        m_symbolArray.push_back(symbol);
    }

    return (stream.CheckState());
}

// Symbol Functions
/// Adds a symbol to the symbol array
/// @param symbolType Type of symbol to add (see symbolTypes enum for values)
/// @param activeSymbol Symbol that must be active for the symbol to be
/// triggered (see activeSymbols enum for values)
/// @param repeatNumber Repeat number that must be active for the symbol to be
/// triggered (0 = none)
/// @return True if the symbol was added, false if not
bool Direction::AddSymbol(uint8_t symbolType, uint8_t activeSymbol,
    uint8_t repeatNumber)
{
    //------Last Checked------//
    // - Jan 11, 2005
    CHECK_THAT(IsValidSymbolType(symbolType), false);
    CHECK_THAT(IsValidActiveSymbol(activeSymbol), false);
    CHECK_THAT(IsValidRepeatNumber(repeatNumber), false);
    
    // Can't add anymore symbols
    if (GetSymbolCount() == MAX_SYMBOLS)
        return (false);

    // Add a symbol to the end of the array, then set the data    
    m_symbolArray.push_back(0);
    return (SetSymbol(GetSymbolCount() - 1, symbolType, activeSymbol,
        repeatNumber));
}

/// Sets the data for an existing symbol in the symbol array
/// @param index Index of the symbol to set the data for
/// @param symbolType Type of symbol (see symbolTypes enum for values)
/// @param activeSymbol Symbol that must be active for the symbol to be
/// triggered (see activeSymbols enum for values)
/// @param repeatNumber Repeat number that must be active for the symbol to be
/// triggered (0 = none)
/// @return True if the symbol data was set, false if not
bool Direction::SetSymbol(uint32_t index, uint8_t symbolType,
    uint8_t activeSymbol, uint8_t repeatNumber)
{
    //------Last Checked------//
    // - Jan 11, 2005
    CHECK_THAT(IsValidSymbolIndex(index), false);
    CHECK_THAT(IsValidSymbolType(symbolType), false);
    CHECK_THAT(IsValidActiveSymbol(activeSymbol), false);
    CHECK_THAT(IsValidRepeatNumber(repeatNumber), false);
    
    uint16_t symbol = (uint16_t)(symbolType << 8);
    symbol |= (uint16_t)(activeSymbol << 6);
    symbol |= (uint16_t)repeatNumber;
   
    m_symbolArray[index] = symbol;
    
    return (true);
}

/// Gets the symbol stored in the nth index of the symbol array
/// @param index Index of the symbol to get
/// @param symbolType Holds the symbol type return value
/// @param activeSymbol Holds the active symbol return value
/// @param repeatNumber Holds the repeat number return value
/// @return True if the direction data was retrieved, false if not
bool Direction::GetSymbol(uint32_t index, uint8_t& symbolType,
    uint8_t& activeSymbol, uint8_t& repeatNumber) const
{
    //------Last Checked------//
    // - Jan 11, 2005
    CHECK_THAT(IsValidSymbolIndex(index), false);
   
    symbolType = activeSymbol = repeatNumber = 0;
    
    symbolType = (uint8_t)((m_symbolArray[index] & symbolTypeMask) >> 8);
    activeSymbol = (uint8_t)((m_symbolArray[index] & activeSymbolMask) >> 6);
    repeatNumber = (uint8_t)(m_symbolArray[index] & repeatNumberMask);
    
    return (true);
}

/// Determines if a symbol in the symbol array is a given type
/// @param index Index of the symbol
/// @param symbolType Type of symbol to test against
/// @return True if the symbol is of the type, false if not
bool Direction::IsSymbolType(uint32_t index, uint8_t symbolType) const
{
    //------Last Checked------//
    // - Jan 11, 2005
    CHECK_THAT(IsValidSymbolIndex(index), false);
    CHECK_THAT(IsValidSymbolType(symbolType), false);
    
    uint8_t type = 0;
    uint8_t activeSymbol = 0;
    uint8_t repeatNumber = 0;
    if (!GetSymbol(index, type, activeSymbol, repeatNumber))
        return (false);
        
    return (type == symbolType);
}

/// Removes a symbol from the symbol array
/// @param index Index of the symbol to remove
/// @return True if the symbol was removed, false if not
bool Direction::RemoveSymbolAtIndex(uint32_t index)
{
    //------Last Checked------//
    // - Jan 11, 2005
    CHECK_THAT(IsValidSymbolIndex(index), false);
 
    m_symbolArray.erase(m_symbolArray.begin() + index);
    
    return (true);
}

/// Deletes the contents (and frees the memory) of the symbol array
void Direction::DeleteSymbolArrayContents()
{
    //------Last Checked------//
    // - Jan 11, 2005
    m_symbolArray.clear();
}

/// Gets a text representation of a symbol
/// @param index Index of the symbol to get the text for
/// @return Text representation of the symbol
std::string Direction::GetText(uint32_t index) const
{
    uint8_t symbolType = 0;
    uint8_t activeSymbol = 0;
    uint8_t repeatNumber = 0;
    if (!GetSymbol(index, symbolType, activeSymbol, repeatNumber))
        return "";

    CHECK_THAT(IsValidSymbolType(symbolType), "");
    
    return (directionText[symbolType]);
}
