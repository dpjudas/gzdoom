/*
** music_libsndfile.cpp
** Uses libsndfile for streaming music formats
**
**---------------------------------------------------------------------------
** Copyright 2017 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

// HEADER FILES ------------------------------------------------------------

#include "i_musicinterns.h"
#include "c_cvars.h"
#include "critsec.h"
#include "v_text.h"
#include "files.h"
#include "templates.h"
#include "sndfile_decoder.h"
#include "mpg123_decoder.h"

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

class SndFileSong : public StreamSong
{
public:
	SndFileSong(FileReader *reader, SoundDecoder *decoder, uint32_t loop_start, uint32_t loop_end);
	~SndFileSong();
	bool SetSubsong(int subsong);
	void Play(bool looping, int subsong);
	FString GetStats();
	
protected:
	FCriticalSection CritSec;
	FileReader *Reader;
	SoundDecoder *Decoder;
	int Channels;
	int SampleRate;
	
	uint32_t Loop_Start;
	uint32_t Loop_End;

	int CalcSongLength();

	static bool Read(SoundStream *stream, void *buff, int len, void *userdata);
};

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

// PUBLIC DATA DEFINITIONS -------------------------------------------------

// PRIVATE DATA DEFINITIONS ------------------------------------------------

// CODE --------------------------------------------------------------------

//==========================================================================
//
// GME_OpenSong
//
//==========================================================================

MusInfo *SndFile_OpenSong(FileReader &fr)
{
	uint8_t signature[4];
	
	fr.Seek(0, SEEK_SET);
	fr.Read(signature, 4);
	uint32_t loop_start = 0, loop_end = ~0u;
	
	if (!memcmp(signature, "OggS", 4) || !memcmp(signature, "fLaC", 4))
	{
		// Todo: Read loop points from metadata
		
		
		// ms to samples.
		//size_t smp_offset = ms? (size_t)((double)ms_offset / 1000. * SndInfo.samplerate) : ms_offset;
		
	}
	fr.Seek(0, SEEK_SET);
	auto decoder = SoundRenderer::CreateDecoder(&fr);
	if (decoder == nullptr) return nullptr;
	return new SndFileSong(&fr, decoder, loop_start, loop_end);
}

//==========================================================================
//
// SndFileSong - Constructor
//
//==========================================================================

SndFileSong::SndFileSong(FileReader *reader, SoundDecoder *decoder, uint32_t loop_start, uint32_t loop_end)
{
	ChannelConfig iChannels;
	SampleType Type;
	
	decoder->getInfo(&SampleRate, &iChannels, &Type);
	
	Loop_Start = loop_start;
	Loop_End = clamp<uint32_t>(loop_end, 0, (uint32_t)decoder->getSampleLength());
	Reader = reader;
	Decoder = decoder;
	Channels = iChannels == ChannelConfig_Stereo? 2:1;
	m_Stream = GSnd->CreateStream(Read, 32*1024, iChannels == ChannelConfig_Stereo? 0 : SoundStream::Mono, SampleRate, this);
}

//==========================================================================
//
// SndFileSong - Destructor
//
//==========================================================================

SndFileSong::~SndFileSong()
{
	Stop();
	if (m_Stream != nullptr)
	{
		delete m_Stream;
		m_Stream = nullptr;
	}
	if (Decoder != nullptr)
	{
		delete Decoder;
	}
	if (Reader != nullptr)
	{
		delete Reader;
	}
}


//==========================================================================
//
// SndFileSong :: Play
//
//==========================================================================

void SndFileSong::Play(bool looping, int track)
{
	m_Status = STATE_Stopped;
	m_Looping = looping;
	if (m_Stream->Play(looping, 1))
	{
		m_Status = STATE_Playing;
	}
}

//==========================================================================
//
// SndFileSong :: SetSubsong
//
//==========================================================================

bool SndFileSong::SetSubsong(int track)
{
	return false;
}

//==========================================================================
//
// SndFileSong :: GetStats
//
//==========================================================================

FString SndFileSong::GetStats()
{
	FString out;
	
	size_t SamplePos;
	
	SamplePos = Decoder->getSampleOffset();
	int time = int (SamplePos / SampleRate);
	
	out.Format(
		"Track: " TEXTCOLOR_YELLOW "%s, %dHz" TEXTCOLOR_NORMAL
		"  Time:" TEXTCOLOR_YELLOW "%02d:%02d" TEXTCOLOR_NORMAL,
		Channels == 2? "Stereo" : "Mono", SampleRate,
		time/60,
		time % 60);
	return out;
}

//==========================================================================
//
// SndFileSong :: Read													STATIC
//
//==========================================================================

bool SndFileSong::Read(SoundStream *stream, void *vbuff, int ilen, void *userdata)
{
	char *buff = (char*)vbuff;
	SndFileSong *song = (SndFileSong *)userdata;
	song->CritSec.Enter();
	
	size_t len = size_t(ilen);
	size_t currentpos = song->Decoder->getSampleOffset();
	size_t framestoread = len / (song->Channels*2);
	bool err = false;
	if (!song->m_Looping)
	{
		size_t maxpos = song->Decoder->getSampleLength();
		if (currentpos == maxpos)
		{
			memset(buff, 0, len);
			song->CritSec.Leave();
			return false;
		}
		if (currentpos + framestoread > maxpos)
		{
			size_t got = song->Decoder->read(buff, (maxpos - currentpos) * song->Channels * 2);
			memset(buff + got, 0, len - got);
		}
		else
		{
			size_t got = song->Decoder->read(buff, len);
			err = (got != len);
		}
	}
	else
	{
		if (currentpos + framestoread > song->Loop_End)
		{
			size_t endblock = (song->Loop_End - currentpos) * song->Channels * 2;
			err = (song->Decoder->read(buff, endblock) != endblock);
			buff = buff + endblock;
			len -= endblock;
			song->Decoder->seek(song->Loop_Start, false);
		}
		err |= song->Decoder->read(buff, len) != len;
	}
	song->CritSec.Leave();
	return !err;
}
