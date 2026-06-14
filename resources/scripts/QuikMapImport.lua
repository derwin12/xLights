-- QuikMapImport.lua
-- Batch-runs QuikMap (the deterministic, non-AI auto-mapper) against every
-- .zip / .xsqz / .piz / .xsq sequence found in a folder.
--
-- For each source sequence this:
--   1. Creates a brand new (blank) sequence against your current layout,
--      sized to match the source's duration/frame rate.
--   2. Runs QuikMap to import the source's effects into it (mapmethod =
--      'quikmap', media not copied).
--   3. Saves the result as "<name>_QuikMap.xsq" in QuikMapResults\ next to
--      the source, so you can open it later and visually compare the
--      mapping against the original.
--   4. Appends the QuikMap phase summary + detailed per-node mapping report
--      (target -> mapped source [rule]) to a single log file.
--
-- NOTE: this closes whatever sequence you currently have open (without
-- saving any unsaved changes - force-closed) and leaves the last generated
-- sequence open at the end. Save any work in your current sequence before
-- running this.

local sourceFolder = [[F:\ShowFolderQA\TestImports]]
local outputFolder = sourceFolder .. [[\QuikMapResults]]
local logFile = outputFolder .. [[\QuikMapReport.log]]

local listResult = RunCommand('listSequences', { folder = sourceFolder })
if listResult == nil or listResult['sequences'] == nil then
    ShowMessage('Could not list sequences in ' .. sourceFolder)
    return
end

os.execute('mkdir "' .. outputFolder .. '"')

local log = io.open(logFile, 'w')
if log == nil then
    ShowMessage('Could not open log file for writing: ' .. logFile)
    return
end

local count = 0
for _, seq in pairs(listResult['sequences']) do
    local path = seq['path']
    local ext = (path:match('%.([^.]+)$') or ''):lower()
    if ext == 'zip' or ext == 'xsqz' or ext == 'piz' or ext == 'xsq' then
        count = count + 1
        Log('QuikMap import pass: ' .. path)

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
            goto continue
        end

        local result = RunCommand('importXLightsSequence', {
            filename    = path,
            importmedia = 'false',
            mapmethod   = 'quikmap',
            detailedreport = 'true'
        })

        if result == nil or result['worked'] ~= 'true' then
            log:write('FAILED to import: ' .. (result and result['msg'] or 'unknown error') .. '\n\n')
            goto continue
        end

        log:write((result['quikMapSummary'] or '(no summary returned)') .. '\n\n')

        local outName = path:match('([^\\/]+)%.[^.]+$') or ('sequence_' .. tostring(count))
        local outPath = outputFolder .. '\\' .. outName .. '_QuikMap.xsq'
        local saveResult = RunCommand('saveSequence', { seq = outPath })
        if saveResult == nil or saveResult['res'] ~= 200 then
            log:write('FAILED to save result sequence: ' .. (saveResult and saveResult['msg'] or 'unknown error') .. '\n\n')
        else
            log:write('Saved: ' .. outPath .. '\n\n')
        end

        ::continue::
    end
end

log:close()

ShowMessage('QuikMap batch import complete for ' .. count .. ' file(s).\n\nResults saved to:\n' .. outputFolder ..
    '\n\nReport written to:\n' .. logFile)
