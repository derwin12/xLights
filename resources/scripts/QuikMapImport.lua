-- QuikMapImport.lua
-- Batch-runs QuikMap (the deterministic, non-AI auto-mapper) against every
-- .zip / .xsqz / .piz / .xsq sequence found in a folder.
--
-- For each source sequence this:
--   1. Creates a brand new (blank) sequence against your current layout,
--      sized to match the source's duration/frame rate.
--   2. Runs QuikMap to import the source's effects into it (mapmethod =
--      'quikmap', media not copied).
--   3. Saves the QuikMap channel mapping as "<name>.xmap" in QuikMapResults\
--      so it can be reloaded directly for future imports of the same vendor.
--   4. Saves the result sequence as "<name>_QuikMap.xsq" in QuikMapResults\.
--   5. Writes the QuikMap phase summary + detailed per-node mapping report
--      (target -> mapped source [rule]) to its own "<name>_QuikMapReport.log"
--      file in QuikMapResults\, so each source sequence gets a dedicated,
--      uniquely-named report instead of all runs being appended into one
--      shared log.
--
-- NOTE: this closes whatever sequence you currently have open (without
-- saving any unsaved changes - force-closed) and leaves the last generated
-- sequence open at the end. Save any work in your current sequence before
-- running this.

local sourceFolder = [[F:\ShowFolderQA\TestImports]]
local outputFolder = sourceFolder .. [[\QuikMapResults]]

local listResult = RunCommand('listSequences', { folder = sourceFolder })
if listResult == nil or listResult['sequences'] == nil then
    ShowMessage('Could not list sequences in ' .. sourceFolder)
    return
end

os.execute('mkdir "' .. outputFolder .. '"')

local count = 0
local lastLogFile = nil
for _, seq in pairs(listResult['sequences']) do
    local path = seq['path']
    local ext = (path:match('%.([^.]+)$') or ''):lower()
    if ext == 'zip' or ext == 'xsqz' or ext == 'piz' or ext == 'xsq' then
        count = count + 1
        Log('QuikMap import pass: ' .. path)

        local outName = path:match('([^\\/]+)%.[^.]+$') or ('sequence_' .. tostring(count))
        local logFile = outputFolder .. '\\' .. outName .. '_QuikMapReport.log'
        lastLogFile = logFile

        local log = io.open(logFile, 'w')
        if log == nil then
            Log('Could not open log file for writing: ' .. logFile)
            goto continue
        end

        log:write('==================================================\n')
        log:write('Source: ' .. path .. '\n')
        log:write('==================================================\n')

        -- Look up duration/frame rate so the new sequence matches the source.
        local info = RunCommand('getSequenceInfo', { filename = path })
        local durationSecs = 60
        local frameMS = 25
        if info ~= nil and info['duration'] ~= nil then
            durationSecs = math.ceil(tonumber(info['duration']) / 1000)
            if info['frameRate'] ~= nil and tonumber(info['frameRate']) > 0 then
                frameMS = math.floor(1000 / tonumber(info['frameRate']) + 0.5)
            end
        end

        -- Close whatever is open (discarding changes) and start fresh.
        RunCommand('closeSequence', { force = 'true', quiet = 'true' })
        local newResult = RunCommand('newSequence', {
            durationSecs = tostring(durationSecs),
            frameMS       = tostring(frameMS),
            force         = 'true'
        })
        if newResult == nil or newResult['res'] ~= 200 then
            log:write('FAILED to create new sequence: ' .. (newResult and newResult['msg'] or 'unknown error') .. '\n\n')
            log:close()
            goto continue
        end

        local mapPath = outputFolder .. '\\' .. outName .. '.xmap'

        local result = RunCommand('importXLightsSequence', {
            filename    = path,
            importmedia = 'false',
            mapmethod   = 'quikmap',
            detailedreport = 'true',
            savemapfile = mapPath
        })

        if result == nil or result['worked'] ~= 'true' then
            log:write('FAILED to import: ' .. (result and result['msg'] or 'unknown error') .. '\n\n')
            log:close()
            goto continue
        end

        log:write((result['quikMapSummary'] or '(no summary returned)') .. '\n\n')
        log:write('Mapping saved: ' .. mapPath .. '\n\n')

        local outPath = outputFolder .. '\\' .. outName .. '_QuikMap.xsq'
        local saveResult = RunCommand('saveSequence', { seq = outPath })
        if saveResult == nil or saveResult['res'] ~= 200 then
            log:write('FAILED to save result sequence: ' .. (saveResult and saveResult['msg'] or 'unknown error') .. '\n\n')
        else
            log:write('Saved: ' .. outPath .. '\n\n')
        end

        log:close()

        ::continue::
    end
end

ShowMessage('QuikMap batch import complete for ' .. count .. ' file(s).\n\nResults saved to:\n' .. outputFolder ..
    '\n\nEach sequence has its own "<name>_QuikMapReport.log" in that folder.' ..
    (lastLogFile and ('\n\nMost recent report:\n' .. lastLogFile) or ''))
