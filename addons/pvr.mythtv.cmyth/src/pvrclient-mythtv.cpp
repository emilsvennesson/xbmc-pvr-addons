/*
 *      Copyright (C) 2005-2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "pvrclient-mythtv.h"

#include "client.h"
#include "tools.h"

#include <time.h>

using namespace ADDON;
using namespace PLATFORM;

RecordingRule::RecordingRule(const MythRecordingRule &rule)
  : MythRecordingRule(rule)
  , m_parent(0)
{
}

RecordingRule &RecordingRule::operator=(const MythRecordingRule &rule)
{
  MythRecordingRule::operator=(rule);
  clear();
  return *this;
}

bool RecordingRule::operator==(const unsigned long &id)
{
  return id==RecordID();
}

RecordingRule *RecordingRule::GetParent() const
{
  return m_parent;
}

void RecordingRule::SetParent(RecordingRule &parent)
{
  m_parent = &parent;
}

bool RecordingRule::HasOverrideRules() const
{
  return !m_overrideRules.empty();
}

std::vector<RecordingRule*> RecordingRule::GetOverrideRules() const
{
  return m_overrideRules;
}

void RecordingRule::AddOverrideRule(RecordingRule &overrideRule)
{
  m_overrideRules.push_back(&overrideRule);
}

bool RecordingRule::SameTimeslot(RecordingRule &rule) const
{
  time_t starttime = StartTime();
  time_t rStarttime = rule.StartTime();

  switch (rule.Type())
  {
  case MythRecordingRule::NotRecording:
  case MythRecordingRule::SingleRecord:
  case MythRecordingRule::OverrideRecord:
  case MythRecordingRule::DontRecord:
    return rStarttime == starttime && rule.EndTime() == EndTime() && rule.ChannelID() == ChannelID();
  case MythRecordingRule::FindDailyRecord:
  case MythRecordingRule::FindWeeklyRecord:
  case MythRecordingRule::FindOneRecord:
    return rule.Title(false) == Title(false);
  case MythRecordingRule::TimeslotRecord:
    return rule.Title(false) == Title(false) && daytime(&starttime) == daytime(&rStarttime) &&  rule.ChannelID() == ChannelID();
  case MythRecordingRule::ChannelRecord:
    return rule.Title(false) == Title(false) && rule.ChannelID() == ChannelID(); //TODO: dup
  case MythRecordingRule::AllRecord:
    return rule.Title(false) == Title(false); //TODO: dup
  case MythRecordingRule::WeekslotRecord:
    return rule.Title(false) == Title(false) && daytime(&starttime) == daytime(&rStarttime) && weekday(&starttime) == weekday(&rStarttime) && rule.ChannelID() == ChannelID();
  }
  return false;
}

void RecordingRule::push_back(std::pair<PVR_TIMER, MythProgramInfo> &_val)
{
  SaveTimerString(_val.first);
  std::vector<std::pair<PVR_TIMER, MythProgramInfo> >::push_back(_val);
}

void RecordingRule::SaveTimerString(PVR_TIMER &timer)
{
  m_stringStore.push_back(boost::shared_ptr<CStdString>(new CStdString(timer.strTitle)));
  PVR_STRCPY(timer.strTitle,m_stringStore.back()->c_str());
  m_stringStore.push_back(boost::shared_ptr<CStdString>(new CStdString(timer.strDirectory)));
  PVR_STRCPY(timer.strDirectory,m_stringStore.back()->c_str());
  m_stringStore.push_back(boost::shared_ptr<CStdString>(new CStdString(timer.strSummary)));
  PVR_STRCPY(timer.strSummary,m_stringStore.back()->c_str());
}

PVRClientMythTV::PVRClientMythTV()
 : m_con()
 , m_pEventHandler(NULL)
 , m_db()
 , m_fileOps(NULL)
 , m_backendVersion("")
 , m_connectionString("")
 , m_categories()
 , m_EPGstart(0)
 , m_EPGend(0)
 , m_channelGroups()
{
}

PVRClientMythTV::~PVRClientMythTV()
{
  if (m_fileOps)
  {
    delete m_fileOps;
    m_fileOps = NULL;
  }

  if (m_pEventHandler)
  {
    delete m_pEventHandler;
    m_pEventHandler = NULL;
  }
}

void Log(int level, char *msg)
{
  if (msg && level != CMYTH_DBG_NONE)
  {
    bool doLog = g_bExtraDebug;
    addon_log_t loglevel = LOG_DEBUG;
    switch (level)
    {
    case CMYTH_DBG_ERROR:
      loglevel = LOG_ERROR;
      doLog = true;
      break;
    case CMYTH_DBG_WARN:
    case CMYTH_DBG_INFO:
      loglevel = LOG_INFO;
      break;
    case CMYTH_DBG_DETAIL:
    case CMYTH_DBG_DEBUG:
    case CMYTH_DBG_PROTO:
    case CMYTH_DBG_ALL:
      loglevel = LOG_DEBUG;
      break;
    }
    if (XBMC && doLog)
      XBMC->Log(loglevel, "LibCMyth: %s", msg);
  }
}

bool PVRClientMythTV::Connect()
{
  // Setup libcmyth logging
  if (g_bExtraDebug)
    cmyth_dbg_all();
  else
    cmyth_dbg_level(CMYTH_DBG_ERROR);
  cmyth_set_dbg_msgcallback(Log);

  // Create MythTV connection
  m_con = MythConnection(g_szMythHostname, g_iMythPort);
  if (!m_con.IsConnected())
  {
    XBMC->Log(LOG_ERROR,"Failed to connect to MythTV backend on %s:%i", g_szMythHostname.c_str(), g_iMythPort);
    XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(30300));
    return false;
  }

  // Create event handler
  m_pEventHandler = m_con.CreateEventHandler();
  if (!m_pEventHandler)
  {
    XBMC->Log(LOG_ERROR, "Failed to create MythEventHandler");
    XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(30300));
    return false;
  }

  // Create database connection
  m_db = MythDatabase(g_szDBHostname, g_szDBName, g_szDBUser, g_szDBPassword, g_iDBPort);
  if (m_db.IsNull())
  {
    XBMC->Log(LOG_ERROR,"Failed to connect to MythTV database %s@%s:%i with user %s", g_szDBName.c_str(), g_szDBHostname.c_str(), g_iDBPort, g_szDBUser.c_str());
    XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(30301));
    return false;
  }

  // Test database connection
  CStdString db_test;
  if (!m_db.TestConnection(&db_test))
  {
    XBMC->Log(LOG_ERROR,"Failed to connect to MythTV database %s@%s:%i with user %s: %s", g_szDBName.c_str(), g_szDBHostname.c_str(), g_iDBPort, g_szDBUser.c_str(), db_test.c_str());
    XBMC->QueueNotification(QUEUE_ERROR, XBMC->GetLocalizedString(30301));
    return false;
  }

  // Create file operation helper (image caching)
  m_fileOps = new FileOps(m_con);
  m_fileOps->UpdateStorageGroupFileList();

  // Get channel list
  m_channels = m_db.GetChannels();
  if (m_channels.empty())
    XBMC->Log(LOG_INFO,"%s: Empty channel list", __FUNCTION__);

  // Get sources
  m_sources = m_db.GetSources();
  if (m_sources.empty())
    XBMC->Log(LOG_INFO,"%s: Empty source list", __FUNCTION__);

  // Get channel groups
  m_channelGroups = m_db.GetChannelGroups();
  if (m_channelGroups.empty())
    XBMC->Log(LOG_INFO,"%s: No channel groups", __FUNCTION__);

  // Get recordings
  m_recordings = m_con.GetRecordedPrograms();
  if (m_recordings.empty())
    XBMC->Log(LOG_INFO,"%s: No recordings", __FUNCTION__);

  return true;
}

const char *PVRClientMythTV::GetBackendName()
{
  m_backendName.Format("MythTV (%s)", m_con.GetBackendName());
  XBMC->Log(LOG_DEBUG, "GetBackendName: %s", m_backendName.c_str());
  return m_backendName;
}

const char *PVRClientMythTV::GetBackendVersion()
{
  m_backendVersion.Format(XBMC->GetLocalizedString(30100), m_con.GetProtocolVersion(), m_db.GetSchemaVersion());
  XBMC->Log(LOG_DEBUG, "GetBackendVersion: %s", m_backendVersion.c_str());
  return m_backendVersion;
}

const char *PVRClientMythTV::GetConnectionString()
{
  m_connectionString.Format("%s:%i - %s@%s:%i", g_szMythHostname, g_iMythPort, g_szDBName, g_szDBHostname, g_iDBPort);
  XBMC->Log(LOG_DEBUG, "GetConnectionString: %s", m_connectionString.c_str());
  return m_connectionString;
}

bool PVRClientMythTV::GetDriveSpace(long long *iTotal, long long *iUsed)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);
  return m_con.GetDriveSpace(*iTotal, *iUsed);
}

PVR_ERROR PVRClientMythTV::GetEPGForChannel(ADDON_HANDLE handle, const PVR_CHANNEL &channel, time_t iStart, time_t iEnd)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG,"%s - start: %i, end: %i, ChannelID: %i", __FUNCTION__, iStart, iEnd, channel.iUniqueId);

  if (iStart != m_EPGstart && iEnd != m_EPGend)
  {
    m_EPG = m_db.GetGuide(iStart, iEnd);
    if (g_bExtraDebug)
      XBMC->Log(LOG_DEBUG,"%s: Fetching EPG - size: %i", __FUNCTION__, m_EPG.size());
    m_EPGstart = iStart;
    m_EPGend = iEnd;
  }

  for (ProgramList::iterator it = m_EPG.begin(); it != m_EPG.end(); it++)
  {
    if ((unsigned)it->chanid==channel.iUniqueId)
    {
      EPG_TAG tag;
      memset(&tag, 0, sizeof(EPG_TAG));

      tag.iChannelNumber = it->channum;
      tag.startTime = it->starttime;
      tag.endTime = it->endtime;

      CStdString title = it->title;
      CStdString subtitle = it->subtitle;
      if (!subtitle.IsEmpty())
        title += ": " + subtitle;
      tag.strTitle = title;

      tag.strPlot = it->description;
      tag.iUniqueBroadcastId = (tag.startTime << 16) + (tag.iChannelNumber & 0xffff);

      int genre = m_categories.Category(it->category);
      tag.iGenreSubType = genre & 0x0F;
      tag.iGenreType = genre & 0xF0;

      // Unimplemented
      tag.strEpisodeName="";
      tag.strGenreDescription="";
      tag.strIconPath="";
      tag.strPlotOutline="";
      tag.bNotify=false;
      tag.firstAired=0;
      tag.iEpisodeNumber=0;
      tag.iEpisodePartNumber=0;
      tag.iParentalRating=0;
      tag.iSeriesNumber=0;
      tag.iStarRating=0;

      PVR->TransferEpgEntry(handle, &tag);
    }
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

int PVRClientMythTV::GetNumChannels()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);
  return m_channels.size();
}

PVR_ERROR PVRClientMythTV::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - radio: %i", __FUNCTION__, bRadio);

  for (ChannelMap::iterator it = m_channels.begin(); it != m_channels.end(); it++)
  {
    if (it->second.IsRadio() == bRadio && !it->second.IsNull())
    {
      PVR_CHANNEL tag;
      memset(&tag, 0, sizeof(PVR_CHANNEL));

      tag.iUniqueId = it->first;
      tag.iChannelNumber = it->second.NumberInt(); // Use ID instead as MythTV channel number is a string?
      PVR_STRCPY(tag.strChannelName, it->second.Name());
      tag.bIsHidden = !it->second.Visible();
      tag.bIsRadio = it->second.IsRadio();

      CStdString icon = GetArtWork(FileOps::FileTypeChannelIcon, it->second.Icon());
      PVR_STRCPY(tag.strIconPath, icon);

      // Unimplemented
      PVR_STRCPY(tag.strStreamURL, "");
      PVR_STRCPY(tag.strInputFormat, "");
      tag.iEncryptionSystem=0;

      PVR->TransferChannelEntry(handle, &tag);
    }
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

int PVRClientMythTV::GetChannelGroupsAmount() const
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  return m_channelGroups.size();
}

PVR_ERROR PVRClientMythTV::GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - radio: %i", __FUNCTION__, bRadio);

  for (ChannelGroupMap::iterator it = m_channelGroups.begin(); it != m_channelGroups.end(); it++)
  {
    PVR_CHANNEL_GROUP tag;
    memset(&tag, 0, sizeof(PVR_CHANNEL_GROUP));

    PVR_STRCPY(tag.strGroupName, it->first);
    tag.bIsRadio = bRadio;

    for (std::vector<int>::iterator it2 = it->second.begin(); it2 != it->second.end(); it2++)
    {
      if (m_channels.find(*it2) != m_channels.end() && m_channels.at(*it2).IsRadio() == bRadio)
      {
        PVR->TransferChannelGroup(handle, &tag);
        break;
      }
    }
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientMythTV::GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - group: %s", __FUNCTION__, group.strGroupName);

  int i=0;
  for (std::vector<int>::iterator it = m_channelGroups.at(group.strGroupName).begin(); it != m_channelGroups.at(group.strGroupName).end(); it++)
  {
    PVR_CHANNEL_GROUP_MEMBER tag;
    memset(&tag, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));

    if (m_channels.find(*it) != m_channels.end())
    {
      MythChannel chan = m_channels.at(*it);
      if (group.bIsRadio == chan.IsRadio())
      {
        tag.iChannelNumber = i++;
        tag.iChannelUniqueId = chan.ID();
        PVR_STRCPY(tag.strGroupName, group.strGroupName);
        PVR->TransferChannelGroupMember(handle, &tag);
      }
    }
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

int PVRClientMythTV::GetRecordingsAmount(void)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  m_con.Lock();
  m_recordings = m_con.GetRecordedPrograms();
  m_con.Unlock();
  int res = 0;
  for (ProgramInfoMap::iterator it = m_recordings.begin(); it != m_recordings.end(); ++it)
  {
    if (!it->second.IsNull() && IsRecordingVisible(it->second))
      res++;
  }
  return res;
}

PVR_ERROR PVRClientMythTV::GetRecordings(ADDON_HANDLE handle)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  m_con.Lock();
  m_recordings = m_con.GetRecordedPrograms();
  m_fileOps->UpdateStorageGroupFileList();
  m_con.Unlock();
  for (ProgramInfoMap::iterator it = m_recordings.begin(); it != m_recordings.end(); ++it)
  {
    if (!it->second.IsNull() && IsRecordingVisible(it->second))
    {
      PVR_RECORDING tag;
      memset(&tag, 0, sizeof(PVR_RECORDING));

      tag.recordingTime = it->second.StartTime();
      tag.iDuration = it->second.Duration();
      tag.iPlayCount = it->second.IsWatched() ? 1 : 0;

      CStdString id = it->second.StrUID();
      CStdString path = it->second.Path();
      CStdString title = it->second.Title(true);

      PVR_STRCPY(tag.strRecordingId, id);
      PVR_STRCPY(tag.strTitle, title);
      PVR_STRCPY(tag.strPlot, it->second.Description());
      PVR_STRCPY(tag.strChannelName, it->second.ChannelName());

      int genre = m_categories.Category(it->second.Category());
      tag.iGenreSubType = genre&0x0F;
      tag.iGenreType = genre&0xF0;

      // Add recording title to directory to group everything according to its name just like MythTV does
      CStdString strDirectory;
      strDirectory.Format("%s/%s", it->second.RecordingGroup(), it->second.Title(false));
      PVR_STRCPY(tag.strDirectory, strDirectory);

      // Images
      CStdString strIconPath = GetArtWork(FileOps::FileTypeCoverart, title);
      if (strIconPath.IsEmpty())
        strIconPath = m_fileOps->GetPreviewIconPath(path + ".png");

      CStdString strFanartPath = GetArtWork(FileOps::FileTypeFanart, title);

      PVR_STRCPY(tag.strIconPath, strIconPath.c_str());
      PVR_STRCPY(tag.strThumbnailPath, strIconPath.c_str());
      PVR_STRCPY(tag.strFanartPath, strFanartPath.c_str());

      // Unimplemented
      tag.iLifetime = 0;
      tag.iPriority = 0;
      PVR_STRCPY(tag.strPlotOutline, "");
      PVR_STRCPY(tag.strStreamURL, "");

      PVR->TransferRecordingEntry(handle, &tag);
    }
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

bool PVRClientMythTV::IsRecordingVisible(MythProgramInfo &recording)
{
  // Filter out recordings of special storage groups (like LiveTV or Deleted)

  // When  deleting a recording, it might not be deleted immediately but marked as 'pending delete'.
  // Depending on the protocol version the recording is moved to the group Deleted or
  // the 'delete pending' flag is set
  if (recording.RecordingGroup() == "LiveTV" || recording.RecordingGroup() == "Deleted" || recording.IsDeletePending())
  {
    XBMC->Log(LOG_DEBUG, "%s: Ignoring recording %s", __FUNCTION__, recording.Path().c_str());
    return false;
  }

  return true;
}

PVR_ERROR PVRClientMythTV::DeleteRecording(const PVR_RECORDING &recording)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  m_con.Lock();
  ProgramInfoMap::iterator it = m_recordings.find(recording.strRecordingId);
  if (it != m_recordings.end())
  {
    bool ret = m_con.DeleteRecording(it->second);
    if (ret)
    {
      m_recordings.erase(it);
      if (g_bExtraDebug)
        XBMC->Log(LOG_DEBUG, "%s - Deleted recording %s", __FUNCTION__, recording.strRecordingId);
      m_con.Unlock();
      return PVR_ERROR_NO_ERROR;
    }
    else
    {
      XBMC->Log(LOG_DEBUG, "%s - Failed to delete recording %s", __FUNCTION__, recording.strRecordingId);
    }
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - Recording %s does not exist", __FUNCTION__, recording.strRecordingId);
  }
  m_con.Unlock();
  return PVR_ERROR_FAILED;
}

PVR_ERROR PVRClientMythTV::SetRecordingPlayCount(const PVR_RECORDING &recording, int count)
{
  XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  if (count > 1) count = 1;
  if (count < 0) count = 0;

  m_con.Lock();
  ProgramInfoMap::iterator it = m_recordings.find(recording.strRecordingId);
  if (it != m_recordings.end())
  {
    int ret = m_db.SetWatchedStatus(it->second, count > 0);

    if (ret == 1)
    {
      if (g_bExtraDebug)
        XBMC->Log(LOG_DEBUG, "%s - Set watched state for %s", __FUNCTION__, recording.strRecordingId);
      m_con.Unlock();
      PVR->TriggerRecordingUpdate();
      return PVR_ERROR_NO_ERROR;
    }
    else
    {
      XBMC->Log(LOG_DEBUG, "%s - Failed setting watched state for: %s: %d)", __FUNCTION__, recording.strRecordingId, ret);
    }
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - Recording %s does not exist", __FUNCTION__, recording.strRecordingId);
  }
  m_con.Unlock();
  return PVR_ERROR_FAILED;
}

PVR_ERROR PVRClientMythTV::SetRecordingLastPlayedPosition(const PVR_RECORDING &recording, int lastplayedposition)
{
  // MythTV provides it's bookmarks as frame offsets whereas XBMC expects a time offset.
  // The frame offset is calculated by: frameOffset = bookmark * frameRate.
  long long frameOffset = 0;

  if (g_bExtraDebug)
  {
    XBMC->Log(LOG_DEBUG, "%s - Setting Bookmark for: %s to %d", __FUNCTION__, recording.strTitle, lastplayedposition);
  }

  m_con.Lock();
  ProgramInfoMap::iterator it = m_recordings.find(recording.strRecordingId);
  if (it != m_recordings.end())
  {
    // Calculate the frame offset
    frameOffset = (long long)(lastplayedposition * m_db.GetRecordingFrameRate(it->second) / 1000.0f);
    if (frameOffset < 0) frameOffset = 0;
    if (g_bExtraDebug)
    {
      XBMC->Log(LOG_DEBUG, "%s - FrameOffset: %lld)", __FUNCTION__, frameOffset);
    }

    // Write the bookmark
    if (m_con.SetBookmark(it->second, frameOffset))
    {
      if (g_bExtraDebug)
      {
        XBMC->Log(LOG_ERROR, "%s - Setting Bookmark successful", __FUNCTION__);
      }
      m_con.Unlock();
      return PVR_ERROR_NO_ERROR;
    }
    else
    {
      if (g_bExtraDebug)
      {
        XBMC->Log(LOG_ERROR, "%s - Setting Bookmark failed", __FUNCTION__);
      }
    }
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - Recording %s does not exist", __FUNCTION__, recording.strRecordingId);
  }
  m_con.Unlock();
  return PVR_ERROR_FAILED;
}

int PVRClientMythTV::GetRecordingLastPlayedPosition(const PVR_RECORDING &recording)
{
  // MythTV provides it's bookmarks as frame offsets whereas XBMC expects a time offset.
  // The bookmark in seconds is calculated by: bookmark = frameOffset / frameRate.
  int bookmark = 0;

  if (g_bExtraDebug)
  {
    XBMC->Log(LOG_DEBUG, "%s - Reading Bookmark for: %s", __FUNCTION__, recording.strTitle);
  }

  m_con.Lock();
  ProgramInfoMap::iterator it = m_recordings.find(recording.strRecordingId);
  if (it != m_recordings.end())
  {
    long long frameOffset = m_con.GetBookmark(it->second); // returns 0 if no bookmark was found
    if (frameOffset > 0)
    {
      if (g_bExtraDebug)
      {
        XBMC->Log(LOG_DEBUG, "%s - FrameOffset: %lld)", __FUNCTION__, frameOffset);
      }

      float frameRate = (float)m_db.GetRecordingFrameRate(it->second) / 1000.0f;
      if (frameRate > 0)
      {
        bookmark = (int)((float)frameOffset / frameRate);
        if (g_bExtraDebug)
        {
          XBMC->Log(LOG_DEBUG, "%s - Bookmark: %d)", __FUNCTION__, bookmark);
        }
      }
    }
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - Recording %s does not exist", __FUNCTION__, recording.strRecordingId);
    m_con.Unlock();
    return PVR_ERROR_FAILED;
  }

  if (bookmark < 0) bookmark = 0;
  m_con.Unlock();
  return bookmark;
}

int PVRClientMythTV::GetTimersAmount(void)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  RecordingRuleMap timers = m_db.GetRecordingRules();
  return timers.size();
}

PVR_ERROR PVRClientMythTV::GetTimers(ADDON_HANDLE handle)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  RecordingRuleMap rules = m_db.GetRecordingRules();
  m_recordingRules.clear();

  for (RecordingRuleMap::iterator it = rules.begin(); it != rules.end(); it++)
    m_recordingRules.push_back(it->second);

  //Search for modifiers and add links to them
  for (RecordingRuleList::iterator it = m_recordingRules.begin(); it != m_recordingRules.end(); it++)
    if (it->Type() == MythRecordingRule::DontRecord || it->Type() == MythRecordingRule::OverrideRecord)
      for (RecordingRuleList::iterator it2 = m_recordingRules.begin(); it2 != m_recordingRules.end(); it2++)
        if (it2->Type() != MythRecordingRule::DontRecord && it2->Type() != MythRecordingRule::OverrideRecord)
          if (it->SameTimeslot(*it2) && !it->GetParent())
          {
            it2->AddOverrideRule(*it);
            it->SetParent(*it2);
          }

  ProgramInfoMap upcomingRecordings = m_con.GetPendingPrograms();
  for (ProgramInfoMap::iterator it = upcomingRecordings.begin(); it != upcomingRecordings.end(); it++)
  {
    // When deleting a timer from mythweb, it might happen that it's removed from database
    // but it's still present over mythprotocol. Skip those timers, because timers.at would crash.
    if (rules.count(it->second.RecordID()) == 0)
    {
      XBMC->Log(LOG_DEBUG, "%s - Skipping timer that is no more in database", __FUNCTION__);
      continue;
    }

    PVR_TIMER tag;
    memset(&tag, 0, sizeof(PVR_TIMER));

    tag.startTime= it->second.RecordingStartTime();
    tag.endTime = it->second.RecordingEndTime();
    tag.iClientChannelUid = it->second.ChannelID();
    tag.iClientIndex = it->second.RecordID();
    tag.iMarginEnd = rules.at(it->second.RecordID()).EndOffset();
    tag.iMarginStart = rules.at(it->second.RecordID()).StartOffset();
    tag.iPriority = it->second.Priority();
    tag.bIsRepeating = false;
    tag.firstDay = 0;
    tag.iWeekdays = 0;

    int genre = m_categories.Category(it->second.Category());
    tag.iGenreSubType = genre & 0x0F;
    tag.iGenreType = genre & 0xF0;

    // Title
    CStdString title = it->second.Title(true);
    if (title.IsEmpty())
    {
      MythProgram epgProgram;
      title= "%";
      m_db.FindProgram(tag.startTime, tag.iClientChannelUid, title, &epgProgram);
      title = epgProgram.title;
    }
    PVR_STRCPY(tag.strTitle, title);

    // Summary
    PVR_STRCPY(tag.strSummary, it->second.Description());

    // Status
    if (g_bExtraDebug)
      XBMC->Log(LOG_DEBUG,"%s ## - State: %d - ##", __FUNCTION__, it->second.Status());
    switch (it->second.Status())
    {
    case RS_RECORDING:
      tag.state = PVR_TIMER_STATE_RECORDING;
      break;
    case RS_TUNING:
      tag.state = PVR_TIMER_STATE_RECORDING;
      break;
    case RS_ABORTED:
      tag.state = PVR_TIMER_STATE_ABORTED;
      break;
    case RS_RECORDED:
      tag.state = PVR_TIMER_STATE_COMPLETED;
      break;
    case RS_WILL_RECORD:
      tag.state = PVR_TIMER_STATE_SCHEDULED;
      break;
    case RS_UNKNOWN:
    case RS_DONT_RECORD:
    case RS_PREVIOUS_RECORDING:
    case RS_CURRENT_RECORDING:
    case RS_EARLIER_RECORDING:
    case RS_TOO_MANY_RECORDINGS:
    case RS_NOT_LISTED:
    case RS_CONFLICT:
      tag.state = PVR_TIMER_STATE_CONFLICT_NOK;
      break;
    case RS_LATER_SHOWING:
    case RS_REPEAT:
    case RS_INACTIVE:
    case RS_NEVER_RECORD:
    case RS_OFFLINE:
    case RS_OTHER_SHOWING:
    case RS_FAILED:
      tag.state = PVR_TIMER_STATE_ERROR;
      break;
    case RS_TUNER_BUSY:
    case RS_LOW_DISKSPACE:
      tag.state = PVR_TIMER_STATE_ERROR;
      break;
    case RS_CANCELLED:
    case RS_MISSED:
    default:
      tag.state = PVR_TIMER_STATE_CANCELLED;
      break;
    }

    // Unimplemented
    tag.iEpgUid=0;
    tag.iLifetime=0;
    PVR_STRCPY(tag.strDirectory, "");

    // Recording rules
    RecordingRuleList::iterator recRule = std::find(m_recordingRules.begin(), m_recordingRules.end() , it->second.RecordID());
    tag.iClientIndex = ((recRule - m_recordingRules.begin()) << 16) + recRule->size();
    //recRule->SaveTimerString(tag);
    std::pair<PVR_TIMER, MythProgramInfo> rrtmp(tag, it->second);
    recRule->push_back(rrtmp);

    PVR->TransferTimerEntry(handle,&tag);
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientMythTV::AddTimer(const PVR_TIMER &timer)
{
  XBMC->Log(LOG_DEBUG, "%s - title: %s, start: %ld, end: %ld, chanID: %ld", __FUNCTION__, timer.strTitle, timer.startTime, timer.endTime, timer.iClientChannelUid);

  MythRecordingRule rule;
  // Fill rule with timer data
  PVRtoMythRecordingRule(timer, rule);
  // Apply default rule setting from backend
  m_con.DefaultRecordingRule(rule);

  int id = m_db.AddRecordingRule(rule);
  if (id < 0)
    return PVR_ERROR_FAILED;

  if (!m_con.UpdateSchedules(id))
    return PVR_ERROR_FAILED;

  XBMC->Log(LOG_DEBUG, "%s - Done - %i", __FUNCTION__, id);

  // Completion of the scheduling will be signaled by a SCHEDULE_CHANGE event.
  // Thus no need to call TriggerTimerUpdate().

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRClientMythTV::DeleteTimer(const PVR_TIMER &timer, bool bForceDelete)
{
  (void)bForceDelete;

  XBMC->Log(LOG_DEBUG, "%s - title: %s, start: %ld, end: %ld, chanID: %ld", __FUNCTION__, timer.strTitle, timer.startTime, timer.endTime, timer.iClientChannelUid);

  RecordingRule rule = m_recordingRules[(timer.iClientIndex)>>16];
  if (rule.GetParent())
    rule = *rule.GetParent();

  // Delete related override rules
  std::vector<RecordingRule*> overrideRules = rule.GetOverrideRules();
  for (std::vector<RecordingRule*>::iterator it = overrideRules.begin(); it != overrideRules.end(); ++it)
  {
    // Stop recording scheduled by the override rule before delete
    for (std::vector<std::pair<PVR_TIMER, MythProgramInfo> >::iterator ip = (*it)->begin(); ip != (*it)->end(); ++ip)
    {
      XBMC->Log(LOG_DEBUG, "%s - recording %s, status = %i", __FUNCTION__, (ip->second).StrUID().c_str(), (ip->second).Status());
      if ((ip->second).Status() == RS_RECORDING || (ip->second).Status() == RS_TUNING)
      {
        XBMC->Log(LOG_DEBUG, "%s - Stop recording %s", __FUNCTION__, (ip->second).StrUID().c_str());
        m_con.StopRecording(ip->second);
      }
    }
    XBMC->Log(LOG_DEBUG, "%s - Delete recording rule %ld (modifier of rule %ld)", __FUNCTION__, (*it)->RecordID(), rule.RecordID());
    if (!m_db.DeleteRecordingRule((*it)->RecordID()))
      return PVR_ERROR_FAILED;
  }

  // Delete parent rule
  for (std::vector<std::pair<PVR_TIMER, MythProgramInfo> >::iterator ip = rule.begin(); ip != rule.end(); ++ip)
  {
    // Stop recording scheduled by the parent rule
    XBMC->Log(LOG_DEBUG, "%s - recording %s, status = %i", __FUNCTION__, (ip->second).StrUID().c_str(), (ip->second).Status());
    if ((ip->second).Status() == RS_RECORDING || (ip->second).Status() == RS_TUNING)
    {
      XBMC->Log(LOG_DEBUG, "%s - Stop recording %s", __FUNCTION__, (ip->second).StrUID().c_str());
      m_con.StopRecording(ip->second);
    }
  }
  XBMC->Log(LOG_DEBUG, "%s - Delete recording rule %ld", __FUNCTION__, rule.RecordID());
  if (!m_db.DeleteRecordingRule(rule.RecordID()))
    return PVR_ERROR_FAILED;

  m_con.UpdateSchedules(-1);

  XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  PVR->TriggerTimerUpdate();

  return PVR_ERROR_NO_ERROR;
}

void PVRClientMythTV::PVRtoMythRecordingRule(const PVR_TIMER timer, MythRecordingRule &rule)
{
  CStdString category = m_categories.Category(timer.iGenreType);
  rule.SetCategory(category);
  rule.SetChannelID(timer.iClientChannelUid);
  rule.SetCallsign(m_channels.at(timer.iClientChannelUid).Callsign());
  rule.SetDescription(timer.strSummary);
  rule.SetEndOffset(timer.iMarginEnd);
  rule.SetEndTime(timer.endTime);
  rule.SetInactive(timer.state == PVR_TIMER_STATE_ABORTED ||timer.state ==  PVR_TIMER_STATE_CANCELLED);
  rule.SetPriority(timer.iPriority);
  rule.SetStartOffset(timer.iMarginStart);
  rule.SetStartTime((timer.startTime == 0 ? time(NULL) : timer.startTime));
  rule.SetTitle(timer.strTitle, true);
  CStdString title = rule.Title(false);
  // kManualSearch = http://www.gossamer-threads.com/lists/mythtv/dev/155150?search_string=kManualSearch;#155150
  rule.SetSearchType(m_db.FindProgram(timer.startTime, timer.iClientChannelUid, title, NULL) ? MythRecordingRule::NoSearch : MythRecordingRule::ManualSearch);
  rule.SetType(timer.bIsRepeating ? (timer.iWeekdays == 127 ? MythRecordingRule::TimeslotRecord : MythRecordingRule::WeekslotRecord) : MythRecordingRule::SingleRecord);
}

PVR_ERROR PVRClientMythTV::UpdateTimer(const PVR_TIMER &timer)
{
  XBMC->Log(LOG_DEBUG, "%s - title: %s, start: %ld, end: %ld, chanID: %ld", __FUNCTION__, timer.strTitle, timer.startTime, timer.endTime, timer.iClientChannelUid);

  RecordingRule oldRule = m_recordingRules[(timer.iClientIndex) >> 16];
  PVR_TIMER oldTimer = oldRule[(timer.iClientIndex) & 0xffff].first;
  {
    bool createNewRule = false;
    bool createNewOverrideRule = false;
    MythRecordingRule rule;
    PVRtoMythRecordingRule(timer, rule);
    rule.SetDescription(oldTimer.strSummary); // Fix broken description
    rule.SetInactive(false);
    rule.SetRecordID(oldRule.RecordID());

    // These should trigger a manual search or a new rule
    if (oldTimer.iClientChannelUid != timer.iClientChannelUid ||
      oldTimer.endTime != timer.endTime ||
      oldTimer.startTime != timer.startTime ||
      oldTimer.startTime != timer.startTime ||
      strcmp(oldTimer.strTitle, timer.strTitle) ||
      timer.bIsRepeating
      )
      createNewRule = true;

    // Change type
    if (oldTimer.state != timer.state)
    {
      if (rule.Type() != MythRecordingRule::SingleRecord && !createNewRule)
      {
        if (timer.state == PVR_TIMER_STATE_ABORTED || timer.state == PVR_TIMER_STATE_CANCELLED)
          rule.SetType(MythRecordingRule::DontRecord);
        else
          rule.SetType(MythRecordingRule::OverrideRecord);
        createNewOverrideRule = true;
      }
      else
        rule.SetInactive(timer.state == PVR_TIMER_STATE_ABORTED || timer.state == PVR_TIMER_STATE_CANCELLED);
    }

    // These can be updated without triggering a new rule
    if (oldTimer.iMarginEnd != timer.iMarginEnd ||
      oldTimer.iPriority != timer.iPriority ||
      oldTimer.iMarginStart != timer.iMarginStart)
      createNewOverrideRule = true;

    CStdString title = timer.strTitle;
    if (createNewRule)
      rule.SetSearchType(m_db.FindProgram(timer.startTime, timer.iClientChannelUid, title, NULL) ? MythRecordingRule::NoSearch : MythRecordingRule::ManualSearch);
    if (createNewOverrideRule && oldRule.SearchType() == MythRecordingRule::ManualSearch)
      rule.SetSearchType(MythRecordingRule::ManualSearch);

    if (oldRule.Type() == MythRecordingRule::DontRecord || oldRule.Type() == MythRecordingRule::OverrideRecord)
      createNewOverrideRule = false;

    if (createNewRule && oldRule.Type() != MythRecordingRule::SingleRecord)
    {
      if (!m_db.AddRecordingRule(rule))
        return PVR_ERROR_FAILED;

      MythRecordingRule mtold;
      PVRtoMythRecordingRule(oldTimer, mtold);
      mtold.SetType(MythRecordingRule::DontRecord);
      mtold.SetInactive(false);
      mtold.SetRecordID(oldRule.RecordID());
      int id = oldRule.RecordID();
      if (oldRule.Type() == MythRecordingRule::DontRecord || oldRule.Type() == MythRecordingRule::OverrideRecord)
        m_db.UpdateRecordingRule(mtold);
      else
        id = m_db.AddRecordingRule(mtold); // Blocks old record rule
      m_con.UpdateSchedules(id);
    }
    else if (createNewOverrideRule &&  oldRule.Type() != MythRecordingRule::SingleRecord )
    {
      if (rule.Type() != MythRecordingRule::DontRecord && rule.Type() != MythRecordingRule::OverrideRecord)
        rule.SetType(MythRecordingRule::OverrideRecord);
      if (!m_db.AddRecordingRule(rule))
        return PVR_ERROR_FAILED;
    }
    else
    {
      if (!m_db.UpdateRecordingRule(rule))
        return PVR_ERROR_FAILED;
    }
    m_con.UpdateSchedules(rule.RecordID());
  }

  XBMC->Log(LOG_DEBUG,"%s - Done", __FUNCTION__);
  return PVR_ERROR_NO_ERROR;
}

bool PVRClientMythTV::OpenLiveStream(const PVR_CHANNEL &channel)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG,"%s - chanID: %i, channumber: %i", __FUNCTION__, channel.iUniqueId, channel.iChannelNumber);

  CLockObject lock(m_lock);
  if (m_rec.IsNull())
  {
    // Suspend fileOps to avoid connection hang
    if (m_fileOps->IsRunning())
      m_fileOps->Suspend();

    // Enable playback mode: Keep quiet on connection
    if (m_pEventHandler)
    {
      m_pEventHandler->EnablePlayback();
    }

    MythChannel chan = m_channels.at(channel.iUniqueId);
    for (std::vector<int>::iterator it = m_sources.at(chan.SourceID()).begin(); it != m_sources.at(chan.SourceID()).end(); it++)
    {
      m_rec = m_con.GetRecorder(*it);
      if (m_rec.ID() > 0 && !m_rec.IsRecording() && m_rec.IsTunable(chan))
      {
        if (g_bExtraDebug)
          XBMC->Log(LOG_DEBUG,"%s: Opening new recorder %i", __FUNCTION__, m_rec.ID());

        if (m_pEventHandler)
        {
          m_pEventHandler->SetRecorder(m_rec);
        }
        if (m_rec.SpawnLiveTV(chan))
          return true;
      }
      m_rec = MythRecorder();
      if (m_pEventHandler)
      {
        m_pEventHandler->SetRecorder(m_rec); // Redundant
      }
    }

    // Disable playback mode: Allow all
    if (m_pEventHandler)
    {
      m_pEventHandler->DisablePlayback();
    }

    // Resume fileOps
    m_fileOps->Resume();

    XBMC->Log(LOG_ERROR,"%s - Failed to open live stream", __FUNCTION__);

    return false;
  }
  else
  {
    if (g_bExtraDebug)
      XBMC->Log(LOG_DEBUG,"%s - Done", __FUNCTION__);

    return true;
  }
}


void PVRClientMythTV::CloseLiveStream()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  CLockObject lock(m_lock);

  if (m_pEventHandler)
    m_pEventHandler->PreventLiveChainUpdate();

  if (!m_rec.Stop())
    XBMC->Log(LOG_NOTICE, "%s - Stop live stream failed", __FUNCTION__);
  m_rec = MythRecorder();

  if (m_pEventHandler)
  {
    m_pEventHandler->SetRecorder(m_rec);
    m_pEventHandler->DisablePlayback();
    m_pEventHandler->AllowLiveChainUpdate();
  }

  // Resume fileOps
  m_fileOps->Resume();

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return;
}

int PVRClientMythTV::ReadLiveStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - size: %i", __FUNCTION__, iBufferSize);

  CLockObject lock(m_lock);

  if (m_rec.IsNull())
    return -1;

  int dataread = m_rec.ReadLiveTV(pBuffer, iBufferSize);
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s: Read %i Bytes", __FUNCTION__, dataread);
  else if (dataread==0)
    XBMC->Log(LOG_INFO, "%s: Read 0 Bytes!", __FUNCTION__);
  return dataread;
}

int PVRClientMythTV::GetCurrentClientChannel()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  CLockObject lock(m_lock);

  if (m_rec.IsNull())
    return -1;

  MythProgramInfo currentProgram = m_rec.GetCurrentProgram();
  return currentProgram.ChannelID();
}

bool PVRClientMythTV::SwitchChannel(const PVR_CHANNEL &channelinfo)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - chanID: %i", __FUNCTION__, channelinfo.iUniqueId);

  bool retval = false;

  //Close current live stream for reopening
  //Keep playback mode enabled
  if (m_pEventHandler)
    m_pEventHandler->PreventLiveChainUpdate();

  retval = m_rec.Stop();
  m_rec = MythRecorder();

  if (m_pEventHandler)
  {
    m_pEventHandler->SetRecorder(m_rec);
    m_pEventHandler->AllowLiveChainUpdate();
  }
  //Try to reopen live stream
  if (retval)
    retval = OpenLiveStream(channelinfo);

  if (!retval)
  {
    XBMC->Log(LOG_ERROR, "%s - Failed to reopening Livestream", __FUNCTION__);
    m_fileOps->Resume();
  }

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return retval;
}

long long PVRClientMythTV::SeekLiveStream(long long iPosition, int iWhence)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - pos: %i, whence: %i", __FUNCTION__, iPosition, iWhence);

  CLockObject lock(m_lock);

  if (m_rec.IsNull())
    return -1;

  int whence;
  if (iWhence == SEEK_SET)
    whence = WHENCE_SET;
  else if (iWhence == SEEK_CUR)
    whence = WHENCE_CUR;
  else
    whence = WHENCE_END;

  long long retval = m_rec.LiveTVSeek(iPosition, whence);

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done - pos: %i", __FUNCTION__, retval);

  return retval;
}

long long PVRClientMythTV::LengthLiveStream()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  CLockObject lock(m_lock);

  if (m_rec.IsNull())
    return -1;

  long long retval = m_rec.LiveTVDuration();

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done - duration: %i", __FUNCTION__, retval);

  return retval;
}

PVR_ERROR PVRClientMythTV::SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  MythSignal signal;
  if (m_pEventHandler)
  {
    signal = m_pEventHandler->GetSignal();
  }

  signalStatus.dAudioBitrate = 0;
  signalStatus.dDolbyBitrate = 0;
  signalStatus.dVideoBitrate = 0;
  signalStatus.iBER = signal.BER();
  signalStatus.iSignal = signal.Signal();
  signalStatus.iSNR = signal.SNR();
  signalStatus.iUNC = signal.UNC();

  CStdString ID;
  ID.Format("Myth Recorder %i", signal.ID());

  CStdString strAdapterStatus = signal.AdapterStatus();
  strcpy(signalStatus.strAdapterName, ID.Buffer());
  strcpy(signalStatus.strAdapterStatus, strAdapterStatus.Buffer());

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);

  return PVR_ERROR_NO_ERROR;
}

bool PVRClientMythTV::OpenRecordedStream(const PVR_RECORDING &recording)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - title: %s, ID: %s, duration: %i", __FUNCTION__, recording.strTitle, recording.strRecordingId, recording.iDuration);

  ProgramInfoMap::iterator it = m_recordings.find(recording.strRecordingId);
  if (it != m_recordings.end())
  {
    // Suspend fileOps to avoid connection hang
    m_fileOps->Suspend();

    // Enable playback mode: Keep quiet on connection
    if (m_pEventHandler)
      m_pEventHandler->EnablePlayback();

    m_file = m_con.ConnectFile(it->second);
    if (m_pEventHandler)
      m_pEventHandler->SetRecordingListener(recording.strRecordingId, m_file);

    // Resume fileOps
    if (m_file.IsNull())
    {
      m_fileOps->Resume();

      // Disable playback mode: Allow all
      if (m_pEventHandler)
        m_pEventHandler->DisablePlayback();
    }

    if (g_bExtraDebug)
      XBMC->Log(LOG_DEBUG, "%s - Done - %i", __FUNCTION__, !m_file.IsNull());

    return !m_file.IsNull();
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - Recording %s does not exist", __FUNCTION__, recording.strRecordingId);
    return false;
  }
}

void PVRClientMythTV::CloseRecordedStream()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  m_file = MythFile();

  if (m_pEventHandler)
    m_pEventHandler->DisablePlayback();

  // Resume fileOps
  m_fileOps->Resume();

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done", __FUNCTION__);
}

int PVRClientMythTV::ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - size: %i curPos: %lld", __FUNCTION__, iBufferSize, (long long)m_file.Position());

  int dataread = m_file.Read(pBuffer, iBufferSize);
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s: Read %i Bytes", __FUNCTION__, dataread);
  else if (dataread == 0)
    XBMC->Log(LOG_INFO, "%s: Read 0 Bytes!", __FUNCTION__);
  return dataread;
}

long long PVRClientMythTV::SeekRecordedStream(long long iPosition, int iWhence)
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - pos: %i, whence: %i", __FUNCTION__, iPosition, iWhence);

  int whence;
  if (iWhence == SEEK_SET)
    whence = WHENCE_SET;
  else if (iWhence == SEEK_CUR)
    whence = WHENCE_CUR;
  else
    whence = WHENCE_END;

  long long retval = m_file.Seek(iPosition, whence);

  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done - pos: %i", __FUNCTION__, retval);

  return retval;
}

long long PVRClientMythTV::LengthRecordedStream()
{
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s", __FUNCTION__);

  long long retval = m_file.Length();
  if (g_bExtraDebug)
    XBMC->Log(LOG_DEBUG, "%s - Done - duration: %i", __FUNCTION__, retval);
  return retval;
}

PVR_ERROR PVRClientMythTV::CallMenuHook(const PVR_MENUHOOK &menuhook)
{
  (void)menuhook;
  return PVR_ERROR_NOT_IMPLEMENTED;
}

bool PVRClientMythTV::GetLiveTVPriority()
{
  if (!m_con.IsNull())
  {
    CStdString value = m_con.GetSetting(m_con.GetHostname(), "LiveTVPriority");
    if (value.compare("1") == 0)
      return true;
    else
      return false;
  }
  return false;
}

void PVRClientMythTV::SetLiveTVPriority(bool enabled)
{
  if (!m_con.IsNull())
  {
    CStdString value = enabled ? "1" : "0";
    m_con.SetSetting(m_con.GetHostname(), "LiveTVPriority", value);
  }
}

CStdString PVRClientMythTV::GetArtWork(FileOps::FileType storageGroup, const CStdString &shwTitle)
{
  if (storageGroup == FileOps::FileTypeCoverart || storageGroup == FileOps::FileTypeFanart)
  {
    return m_fileOps->GetArtworkPath(shwTitle, storageGroup);
  }
  else if (storageGroup == FileOps::FileTypeChannelIcon)
  {
    return m_fileOps->GetChannelIconPath(shwTitle);
  }
  else
  {
    XBMC->Log(LOG_DEBUG, "%s - ## Not a valid storageGroup ##", __FUNCTION__);
    return "";
  }
}
