#include "midiplayer.h"

#include <QSettings>
#include <app/settings.h>
#include <QDebug>

#include <rtmidiwrapper.h>

#include <boost/next_prior.hpp>
#include <boost/foreach.hpp>

#include <painters/caret.h>

#include <powertabdocument/score.h>
#include <powertabdocument/guitar.h>
#include <powertabdocument/generalmidi.h>
#include <powertabdocument/system.h>
#include <powertabdocument/tempomarker.h>
#include <powertabdocument/staff.h>
#include <powertabdocument/position.h>

#include <audio/midievent.h>
#include <audio/playnoteevent.h>
#include <audio/vibratoevent.h>
#include <audio/stopnoteevent.h>
#include <audio/metronomeevent.h>
#include <audio/letringevent.h>
#include <audio/bendevent.h>
#include <audio/repeatcontroller.h>

using std::shared_ptr;
using std::unique_ptr;

MidiPlayer::MidiPlayer(Caret* caret, int playbackSpeed) :
    caret(caret),
    isPlaying(false),
    currentSystemIndex(0),
    activePitchBend(BendEvent::DEFAULT_BEND),
    playbackSpeed(playbackSpeed)
{
    initHarmonicPitches();
}

MidiPlayer::~MidiPlayer()
{
    mutex.lock();
    isPlaying = false;
    mutex.unlock();

    wait();
}

void MidiPlayer::run()
{
    mutex.lock();
    isPlaying = true;
    mutex.unlock();

    const SystemLocation startLocation(caret->getCurrentSystemIndex(),
                                       caret->getCurrentPositionIndex());

    boost::ptr_list<MidiEvent> eventList;
    double timeStamp = 0;

    // go through each system, generate a list of the notes (midi events) from each staff
    // then, sort notes by their start time, and play them in order
    for (currentSystemIndex = 0; currentSystemIndex < caret->getCurrentScore()->GetSystemCount(); ++currentSystemIndex)
    {
        generateMetronome(currentSystemIndex, timeStamp, eventList);

        timeStamp = generateEventsForSystem(currentSystemIndex, timeStamp, eventList);
    }

    eventList.sort();

    playMidiEvents(eventList, startLocation);
}

/// Returns the appropriate note velocity type for the given position/note
PlayNoteEvent::VelocityType getNoteVelocity(const Position* position, const Note* note)
{
    if (note->IsGhostNote())
    {
        return PlayNoteEvent::GHOST_VELOCITY;
    }
    if (note->IsMuted())
    {
        return PlayNoteEvent::MUTED_VELOCITY;
    }
    if (position->HasPalmMuting())
    {
        return PlayNoteEvent::PALM_MUTED_VELOCITY;
    }

    return PlayNoteEvent::DEFAULT_VELOCITY;
}

/// Generates a list of all notes in the given system, by iterating through each position in each staff of the system
/// @return The timestamp of the end of the last event in the system
double MidiPlayer::generateEventsForSystem(uint32_t systemIndex, const double systemStartTime,
                                           boost::ptr_list<MidiEvent>& eventList)
{
    double endTime = systemStartTime;

    shared_ptr<const System> system = caret->getCurrentScore()->GetSystem(systemIndex);

    for (quint32 i = 0; i < system->GetStaffCount(); i++)
    {
        shared_ptr<const Staff> staff = system->GetStaff(i);
        shared_ptr<const Guitar> guitar = caret->getCurrentScore()->GetGuitar(i);

        for (quint32 voice = 0; voice < Staff::NUM_STAFF_VOICES; voice++)
        {
            activePitchBend = BendEvent::DEFAULT_BEND;

            // each note in the staff is given a start time relative to the first note of the staff
            double startTime = systemStartTime;
            
            bool letRingActive = false;

            for (quint32 j = 0; j < staff->GetPositionCount(voice); j++)
            {
                Position* position = staff->GetPosition(voice, j);

                const double currentTempo = getCurrentTempo(position->GetPosition());

                double duration = calculateNoteDuration(position); // each note at a position has the same duration

                if (position->IsRest())
                {
                    // for whole rests, they must last for the entire bar, regardless of time signature
                    if (position->GetDurationType() == 1)
                    {
                        duration = getWholeRestDuration(system, staff, position, duration);
                    }

                    startTime += duration;
                    endTime = std::max(endTime, startTime);
                    continue;
                }

                if (position->IsAcciaccatura()) // grace note
                {
                    duration = GRACE_NOTE_DURATION;
                    startTime -= duration;
                }

                const quint32 positionIndex = position->GetPosition();

                // If the position has an arpeggio, sort the notes by string in the specified direction.
                // This is so the notes can be played in the correct order, with a slight delay between each
                if (position->HasArpeggioDown())
                {
                    position->SortNotesDown();
                }
                else if (position->HasArpeggioUp())
                {
                    position->SortNotesUp();
                }

                // vibrato events (these apply to all notes in the position)
                if (position->HasVibrato() || position->HasWideVibrato())
                {
                    VibratoEvent::VibratoType type = position->HasVibrato() ? VibratoEvent::NORMAL_VIBRATO :
                                                                              VibratoEvent::WIDE_VIBRATO;

                    // add vibrato event, and an event to turn of the vibrato after the note is done
                    eventList.push_back(new VibratoEvent(i, startTime, positionIndex, systemIndex,
                                                         VibratoEvent::VIBRATO_ON, type));

                    eventList.push_back(new VibratoEvent(i, startTime + duration, positionIndex,
                                                         systemIndex, VibratoEvent::VIBRATO_OFF));
                }

                // let ring events (applied to all notes in the position)
                if (position->HasLetRing() && !letRingActive)
                {
                    eventList.push_back(new LetRingEvent(i, startTime, positionIndex, systemIndex,
                                                         LetRingEvent::LET_RING_ON));
                    letRingActive = true;
                }
                else if (!position->HasLetRing() && letRingActive)
                {
                    eventList.push_back(new LetRingEvent(i, startTime, positionIndex, systemIndex,
                                                         LetRingEvent::LET_RING_OFF));
                    letRingActive = false;
                }
                // make sure that we end the let ring after the last position in the system
                else if (letRingActive && (j == staff->GetPositionCount(voice) - 1))
                {
                    eventList.push_back(new LetRingEvent(i, startTime + duration, positionIndex, systemIndex,
                                                         LetRingEvent::LET_RING_OFF));
                    letRingActive = false;
                }

                for (quint32 k = 0; k < position->GetNoteCount(); k++)
                {
                    // for arpeggios, delay the start of each note a small amount from the last,
                    // and also adjust the duration correspondingly
                    if (position->HasArpeggioDown() || position->HasArpeggioUp())
                    {
                        startTime += ARPEGGIO_OFFSET;
                        duration -= ARPEGGIO_OFFSET;
                    }

                    const Note* note = position->GetNote(k);

                    uint32_t pitch = getActualNotePitch(note, guitar);

                    const PlayNoteEvent::VelocityType velocity = getNoteVelocity(position, note);

                    // if this note is not tied to the previous note, play the note
                    if (!note->IsTied())
                    {
                        eventList.push_back(new PlayNoteEvent(i, startTime, duration, pitch,
                                                              positionIndex, systemIndex, guitar,
                                                              note->IsMuted(), velocity));
                    }
                    // if the note is tied, make sure that the pitch is the same as the previous note, 
                    // so that the Stop Note event works correctly with harmonics
                    else 
                    {
                        const Note* prevNote = staff->GetAdjacentNoteOnString(Staff::PrevNote, position, note, voice);
                        
                        // TODO - deal with ties that wrap across systems
                        if (prevNote)
                        {
                            pitch = getActualNotePitch(prevNote, guitar);
                        }
                    }

                    // generate all events that involve pitch bends
                    {
                        std::vector<BendEventInfo> bendEvents;

                        if (note->HasSlide())
                        {
                            generateSlides(bendEvents, startTime, duration, currentTempo, note);
                        }

                        if (note->HasBend())
                        {
                            generateBends(bendEvents, startTime, duration, currentTempo, note);
                        }

                        BOOST_FOREACH(const BendEventInfo& event, bendEvents)
                        {
                            eventList.push_back(new BendEvent(i, event.timestamp, positionIndex,
                                                              systemIndex, event.pitchBendAmount));
                        }
                    }

                    // Perform tremolo picking or trills - they work identically, except trills alternate between two pitches
                    if (position->HasTremoloPicking() || note->HasTrill())
                    {
                        // each note is a 32nd note
                        const double tremPickNoteDuration = currentTempo / 8.0;
                        const int numNotes = duration / tremPickNoteDuration;

                        // find the other pitch to alternate with (this is just the same pitch for tremolo picking)
                        uint32_t otherPitch = pitch;
                        if (note->HasTrill())
                        {
                            uint8_t otherFret = 0;
                            note->GetTrill(otherFret);
                            otherPitch = pitch + (note->GetFretNumber() - otherFret);
                        }

                        for (int k = 0; k < numNotes; ++k)
                        {
                            const double currentStartTime = startTime + k * tremPickNoteDuration;

                            eventList.push_back(new StopNoteEvent(i, currentStartTime, positionIndex,
                                                                  systemIndex, pitch));

                            // alternate to the other pitch (this has no effect for tremolo picking)
                            std::swap(pitch, otherPitch);

                            eventList.push_back(new PlayNoteEvent(i, currentStartTime, tremPickNoteDuration, pitch,
                                                                  positionIndex, systemIndex, guitar,
                                                                  note->IsMuted(), velocity));
                        }
                    }

                    bool tiedToNextNote = false;
                    // check if this note is tied to the next note
                    {
                        const Note* nextNote = staff->GetAdjacentNoteOnString(Staff::NextNote, position, note, voice);
                        if (nextNote && nextNote->IsTied())
                        {
                            tiedToNextNote = true;
                        }
                    }

                    // end the note, unless we are tied to the next note
                    if (!note->HasTieWrap() && !tiedToNextNote)
                    {
                        double noteLength = duration;

                        if (position->IsStaccato())
                        {
                            noteLength /= 2.0;
                        }
                        else if (position->HasPalmMuting())
                        {
                            noteLength /= 1.15;
                        }

                        eventList.push_back(new StopNoteEvent(i, startTime + noteLength,
                                                              positionIndex, systemIndex, pitch));
                    }
                }

                startTime += duration;

                endTime = std::max(endTime, startTime);
            }
        }
    }

    return endTime;
}

// The events are already in order of occurrence, so just play them one by one
// startLocation is used to identify the starting position to begin playback from
void MidiPlayer::playMidiEvents(boost::ptr_list<MidiEvent>& eventList, SystemLocation startLocation)
{
    RtMidiWrapper rtMidiWrapper;

    // set the port for RtMidi
    QSettings settings;
    rtMidiWrapper.initialize(settings.value(Settings::MIDI_PREFERRED_PORT,
                                            Settings::MIDI_PREFFERED_PORT_DEFAULT).toInt());

    // set pitch bend settings for each channel to one octave
    for (uint8_t i = 0; i < midi::NUM_MIDI_CHANNELS_PER_PORT; i++)
    {
        rtMidiWrapper.setPitchBendRange(i, 12);
    }

    RepeatController repeatController(caret->getCurrentScore());

    SystemLocation currentLocation;

    auto activeEvent = eventList.begin();

    while (activeEvent != eventList.end())
    {
        {
            QMutexLocker locker(&mutex);
            Q_UNUSED(locker);

            if (!isPlaying)
            {
                return;
            }
        }

        const SystemLocation eventLocation(activeEvent->getSystemIndex(), activeEvent->getPositionIndex());

        // if we haven't reached the starting position yet, keep going
        if (eventLocation < startLocation)
        {
            ++activeEvent;
            continue;
        }
        // if we just reached the starting position, update the system index explicitly
        // to avoid the "currentPosition = 0" effect of a normal system change
        else if (eventLocation == startLocation)
        {
            emit playbackSystemChanged(startLocation.getSystemIndex());
            currentLocation.setSystemIndex(startLocation.getSystemIndex());
            startLocation = SystemLocation(0, 0);
        }

        // if we've moved to a new position, move the caret
        if (eventLocation.getPositionIndex() > currentLocation.getPositionIndex())
        {
            currentLocation.setPositionIndex(eventLocation.getPositionIndex());
            emit playbackPositionChanged(currentLocation.getPositionIndex());
        }

        // moving on to a new system, so we need to reset the position to 0 to ensure
        // playback begins at the start of the staff
        if (eventLocation.getSystemIndex() != currentLocation.getSystemIndex())
        {
            currentLocation.setSystemIndex(eventLocation.getSystemIndex());
            currentLocation.setPositionIndex(0);
            emit playbackSystemChanged(currentLocation.getSystemIndex());
        }

        SystemLocation newLocation;
        if (repeatController.checkForRepeat(currentLocation, newLocation))
        {
            qDebug() << "Moving to: " << newLocation.getSystemIndex()
                     << ", " << newLocation.getPositionIndex();
            qDebug() << "From position: " << currentLocation.getSystemIndex()
                     << ", " << currentLocation.getPositionIndex()
                     << " at " << activeEvent->getStartTime();

            startLocation = newLocation;
            currentLocation = SystemLocation(0, 0);
            emit playbackSystemChanged(startLocation.getSystemIndex());
            emit playbackPositionChanged(startLocation.getPositionIndex());
            activeEvent = eventList.begin();
            continue;
        }

        activeEvent->performEvent(rtMidiWrapper);

        // add delay between this event and the next one
        auto nextEvent = boost::next(activeEvent);
        if (nextEvent != eventList.end())
        {
            const int sleepDuration = abs(nextEvent->getStartTime() - activeEvent->getStartTime());

            mutex.lock();
            const double speedShiftFactor = 100.0 / playbackSpeed; // slow down or speed up playback
            mutex.unlock();

            usleep(1000 * sleepDuration * speedShiftFactor);
        }
        else // last note
        {
            usleep(1000 * activeEvent->getDuration());
        }

        ++activeEvent;
    }
}

// Finds the active tempo marker
TempoMarker* MidiPlayer::getCurrentTempoMarker(const quint32 positionIndex) const
{
    Score* currentScore = caret->getCurrentScore();

    TempoMarker* currentTempoMarker = NULL;

    // find the active tempo marker
    for(quint32 i = 0; i < currentScore->GetTempoMarkerCount(); i++)
    {
        TempoMarker* temp = currentScore->GetTempoMarker(i);
        if (temp->GetSystem() <= currentSystemIndex &&
            temp->GetPosition() <=  positionIndex &&
            !temp->IsAlterationOfPace()) // TODO - properly support alterations of pace
        {
            currentTempoMarker = temp;
        }
    }

    return currentTempoMarker;
}

/// Returns the current tempo (duration of a quarter note in milliseconds)
double MidiPlayer::getCurrentTempo(const quint32 positionIndex) const
{
    TempoMarker* tempoMarker = getCurrentTempoMarker(positionIndex);

    double bpm = TempoMarker::DEFAULT_BEATS_PER_MINUTE; // default tempo in case there is no tempo marker in the score
    double beatType = TempoMarker::DEFAULT_BEAT_TYPE;

    if (tempoMarker != NULL)
    {
        bpm = tempoMarker->GetBeatsPerMinute();
        Q_ASSERT(bpm != 0);

        beatType = tempoMarker->GetBeatType();
    }

    // convert bpm to millisecond duration
    return (60.0 / bpm * 1000.0 * (TempoMarker::quarter / beatType));
}

double MidiPlayer::calculateNoteDuration(const Position* currentPosition) const
{
    const double tempo = getCurrentTempo(currentPosition->GetPosition());

    return currentPosition->GetDuration() * tempo;
}

double MidiPlayer::getWholeRestDuration(shared_ptr<const System> system, shared_ptr<const Staff> staff,
                                        const Position* position, double originalDuration) const
{
    Barline* prevBarline = system->GetPrecedingBarline(position->GetPosition());

    // if the whole rest is not the only item in the bar, treat it like a regular rest
    if (!staff->IsOnlyPositionInBar(position, system))
    {
        return originalDuration;
    }

    const TimeSignature& currentTimeSignature = prevBarline->GetTimeSignatureConstRef();

    const double tempo = getCurrentTempo(position->GetPosition());
    double beatDuration = currentTimeSignature.GetBeatAmount();
    double duration = tempo * 4.0 / beatDuration;
    int numBeats = currentTimeSignature.GetBeatsPerMeasure();
    duration *= numBeats;

    return duration;
}

// initialize the mapping of frets to pitch offsets (counted in half-steps or frets)
// e.g. The natural harmonic at the 7th fret is an octave and a fifth - 19 frets - above the pitch of the open string
void MidiPlayer::initHarmonicPitches()
{
    harmonicPitches[3] = 31;
    harmonicPitches[4] = harmonicPitches[9] = 28;
    harmonicPitches[16] = harmonicPitches[28] = 28;
    harmonicPitches[5] = harmonicPitches[24] = 24;
    harmonicPitches[7] = harmonicPitches[19] = 19;
    harmonicPitches[12] = 12;
}

quint8 MidiPlayer::getHarmonicPitch(const quint8 basePitch, const quint8 fretOffset) const
{
    return basePitch + harmonicPitches[fretOffset];
}

// Generates the metronome ticks
void MidiPlayer::generateMetronome(uint32_t systemIndex, double startTime,
                                   boost::ptr_list<MidiEvent>& eventList) const
{
    shared_ptr<System> system = caret->getCurrentScore()->GetSystem(systemIndex);

    std::vector<const Barline*> barlines;
    system->GetBarlines(barlines);
    barlines.pop_back(); // don't need the end barline

    for (size_t i = 0; i < barlines.size(); i++)
    {
        const Barline* barline = barlines.at(i);
        const TimeSignature& timeSig = barline->GetTimeSignatureConstRef();

        const quint8 numPulses = timeSig.GetPulses();
        const quint8 beatsPerMeasure = timeSig.GetBeatsPerMeasure();
        const quint8 beatValue = timeSig.GetBeatAmount();

        // figure out duration of pulse
        const double tempo = getCurrentTempo(barline->GetPosition());
        double duration = tempo * 4.0 / beatValue;
        duration *= beatsPerMeasure / numPulses;

        const quint32 position = barline->GetPosition();

        for (quint8 j = 0; j < numPulses; j++)
        {
            MetronomeEvent::VelocityType velocity = (j == 0) ? MetronomeEvent::STRONG_ACCENT :
                                                               MetronomeEvent::WEAK_ACCENT;

            eventList.push_back(new MetronomeEvent(METRONOME_CHANNEL, startTime, duration,
                                                   position, systemIndex, velocity));

            startTime += duration;

            eventList.push_back(new StopNoteEvent(METRONOME_CHANNEL, startTime, position,
                                                  systemIndex, MetronomeEvent::METRONOME_PITCH));
        }
    }

    // insert an empty event for the last barline of the system, to trigger any repeat events for that bar
    eventList.push_back(new StopNoteEvent(METRONOME_CHANNEL, startTime,
                                          system->GetEndBarConstRef().GetPosition(),
                                          systemIndex, MetronomeEvent::METRONOME_PITCH));
}

uint32_t MidiPlayer::getActualNotePitch(const Note* note, shared_ptr<const Guitar> guitar) const
{
    const Tuning& tuning = guitar->GetTuningConstRef();
    
    const quint32 openStringPitch = tuning.GetNote(note->GetString()) + guitar->GetCapo();
    quint32 pitch = openStringPitch + note->GetFretNumber();
    
    if (note->IsNaturalHarmonic())
    {
        pitch = getHarmonicPitch(openStringPitch, note->GetFretNumber());
    }
    
    if (note->HasTappedHarmonic())
    {
        uint8_t tappedFret = 0;
        note->GetTappedHarmonic(tappedFret);
        pitch = getHarmonicPitch(pitch, tappedFret - note->GetFretNumber());
    }
    
    if (note->HasArtificialHarmonic())
    {
        uint8_t key = 0, keyVariation = 0, octaveDiff = 0;
        note->GetArtificialHarmonic(key, keyVariation, octaveDiff);
        
        pitch = (midi::GetMidiNoteOctave(pitch) + octaveDiff + 2) * 12 + key;
    }
    
    return pitch;
}

/// Generates bend events for the given note
void MidiPlayer::generateBends(std::vector<BendEventInfo>& bends, double startTime,
                               double duration, double currentTempo, const Note* note)
{
    uint8_t type = 0, bentPitch = 0, releasePitch = 0, bendDuration = 0, drawStartPoint = 0, drawEndPoint = 0;
    note->GetBend(type, bentPitch, releasePitch, bendDuration, drawStartPoint, drawEndPoint);

    const uint8_t bendAmount = floor(BendEvent::DEFAULT_BEND + bentPitch * BendEvent::BEND_QUARTER_TONE);
    const uint8_t releaseAmount = floor(BendEvent::DEFAULT_BEND + releasePitch * BendEvent::BEND_QUARTER_TONE);

    // perform a pre-bend
    if (type == Note::preBend || type == Note::preBendAndRelease || type == Note::preBendAndHold)
    {
        bends.push_back(BendEventInfo(startTime, bendAmount));
    }

    // perform a normal (gradual) bend
    if (type == Note::normalBend || type == Note::bendAndHold)
    {
        if (bendDuration == 0) // default - bend over 32nd note
        {
            generateGradualBend(bends, startTime, currentTempo / 8.0, BendEvent::DEFAULT_BEND, bendAmount);
        }
        else if (bendDuration == 1) // bend over current note duration
        {
            generateGradualBend(bends, startTime, duration, BendEvent::DEFAULT_BEND, bendAmount);
        }
        // TODO - implement bends that stretch over multiple notes
    }

    // for a "bend and release", bend up to bent pitch, for half the note duration
    if (type == Note::bendAndRelease)
    {
        generateGradualBend(bends, startTime, duration / 2, BendEvent::DEFAULT_BEND, bendAmount);
    }

    // bend back down to the release pitch
    if (type == Note::preBendAndRelease)
    {
        generateGradualBend(bends, startTime, duration, bendAmount, releaseAmount);
    }
    else if (type == Note::bendAndRelease)
    {
        generateGradualBend(bends, startTime + duration / 2, duration / 2, bendAmount, releaseAmount);
    }
    else if (type == Note::gradualRelease)
    {
        generateGradualBend(bends, startTime, duration, activePitchBend, releaseAmount);
    }

    // reset to the release pitch bend value
    if (type == Note::preBend || type == Note::immediateRelease || type == Note::normalBend)
    {
        bends.push_back(BendEventInfo(startTime + duration, releaseAmount));
    }

    if (type == Note::bendAndHold || type == Note::preBendAndHold)
    {
        activePitchBend = bendAmount;
    }
    else
    {
        activePitchBend = releaseAmount;
    }
}

/// Generates a series of BendEvents to perform a gradual bend over the given duration
/// Bends the note from the startBendAmount to the releaseBendAmount over the note duration
void MidiPlayer::generateGradualBend(std::vector<BendEventInfo>& bends, double startTime, double duration,
                                uint8_t startBendAmount, uint8_t releaseBendAmount) const
{
    const int numBendEvents = abs(startBendAmount - releaseBendAmount);
    const double bendEventDuration = duration / numBendEvents;

    for (int i = 1; i <= numBendEvents; i++)
    {
        const double timestamp = startTime + bendEventDuration * i;
        if (startBendAmount < releaseBendAmount)
        {
            bends.push_back(BendEventInfo(timestamp, startBendAmount + i));
        }
        else
        {
            bends.push_back(BendEventInfo(timestamp, startBendAmount - i));
        }
    }
}

MidiPlayer::BendEventInfo::BendEventInfo(double timestamp, uint8_t pitchBendAmount) :
    timestamp(timestamp),
    pitchBendAmount(pitchBendAmount)
{
}

void MidiPlayer::changePlaybackSpeed(int newPlaybackSpeed)
{
    // playback speed may be changed via the main thread during playback
    mutex.lock();
    playbackSpeed = newPlaybackSpeed;
    mutex.unlock();
}

/// Generates slides for the given note
void MidiPlayer::generateSlides(std::vector<BendEventInfo>& bends, double startTime,
                               double noteDuration, double currentTempo, const Note* note)
{
    const int SLIDE_OUT_OF_STEPS = 5;

    const double SLIDE_BELOW_BEND = floor(BendEvent::DEFAULT_BEND -
                                               SLIDE_OUT_OF_STEPS * 2 * BendEvent::BEND_QUARTER_TONE);

    const double SLIDE_ABOVE_BEND = floor(BendEvent::DEFAULT_BEND +
                                          SLIDE_OUT_OF_STEPS * 2 * BendEvent::BEND_QUARTER_TONE);

    if (note->HasSlideOutOf())
    {
        int8_t steps = 0;
        uint8_t type = 0;
        note->GetSlideOutOf(type, steps);

        uint8_t bendAmount = BendEvent::DEFAULT_BEND;

        switch(type)
        {
        case Note::slideOutOfLegatoSlide:
        case Note::slideOutOfShiftSlide:
            bendAmount = floor(BendEvent::DEFAULT_BEND +
                               steps * 2 * BendEvent::BEND_QUARTER_TONE);
            break;

        case Note::slideOutOfDownwards:
            bendAmount = SLIDE_BELOW_BEND;
            break;

        case Note::slideOutOfUpwards:
            bendAmount = SLIDE_ABOVE_BEND;
            break;

        default:
            Q_ASSERT("Unexpected slide type");
            break;
        }

        // start the slide in the last half of the note duration, to make it somewhat more realistic-sounding
        const double slideDuration = noteDuration / 2.0;
        generateGradualBend(bends, startTime + slideDuration, slideDuration,
                            BendEvent::DEFAULT_BEND, bendAmount);

        // reset pitch wheel after note
        bends.push_back(BendEventInfo(startTime + noteDuration, BendEvent::DEFAULT_BEND));
    }

    if (note->HasSlideInto())
    {
        uint8_t type = 0;
        note->GetSlideInto(type);

        uint8_t bendAmount = BendEvent::DEFAULT_BEND;

        switch(type)
        {
        case Note::slideIntoFromBelow:
            bendAmount = SLIDE_BELOW_BEND;
            break;

        case Note::slideIntoFromAbove:
            bendAmount = SLIDE_ABOVE_BEND;
            break;

        default:
            qDebug() << "Unsupported Slide Into type";
            break;
        }

        // slide over a 16th note
        const double slideDuration = currentTempo / 4.0;
        generateGradualBend(bends, startTime, slideDuration, bendAmount, BendEvent::DEFAULT_BEND);
    }
}
