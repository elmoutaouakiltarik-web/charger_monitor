/* English / Italian strings. Technical wording mirrors the field guide.
 * Each entry is [en, it]. A missing key falls back to English, then to the key. */
(function (root) {
  'use strict';
  var L = {
    title: ['Charger Control Room', 'Charger Control Room'],
    subtitle: ['read-only · offline · verifies in your browser', 'sola lettura · offline · verifica nel tuo browser'],
    open_journal: ['Open journal…', 'Apri journal…'],
    demo: ['Demo shift (synthetic)', 'Turno demo (sintetico)'],
    demo_altered: ['Demo with one altered record', 'Demo con un record alterato'],
    theme: ['Theme', 'Tema'],
    theme_auto: ['Auto', 'Auto'], theme_dark: ['Dark', 'Scuro'], theme_light: ['Light', 'Chiaro'],
    tab_overview: ['Overview', 'Panoramica'], tab_fleet: ['Fleet', 'Flotta'], tab_sessions: ['Sessions', 'Sessioni'],
    tab_integrity: ['Integrity', 'Integrità'], tab_live: ['Live', 'Live'],
    foot_readonly: [
      'Read-only. This page cannot command a charger, a bus or a server: it only reads the files you give it, or same-origin status files you name on the Live tab. No telemetry, no external requests, no dependencies.',
      'Sola lettura. Questa pagina non può comandare un caricatore, un bus o un server: legge solo i file che le dai, o i file di stato della stessa origine che indichi nella scheda Live. Nessuna telemetria, nessuna richiesta esterna, nessuna dipendenza.'],
    foot_math: [
      'Energy and money are exact integers (µWh, µEUR) computed with BigInt; the hash chain is checked byte for byte with an independent SHA-256. It re-implements the rules of charger_monitor so an auditor does not have to trust the binary.',
      'Energia e denaro sono interi esatti (µWh, µEUR) calcolati con BigInt; la catena di hash è verificata byte per byte con un SHA-256 indipendente. Reimplementa le regole di charger_monitor, così un auditor non deve fidarsi del binario.'],
    foot_limits: [
      'Not a legal or fiscal meter (MID/Eichrecht), not a safety function, and not proof of who wrote a journal: a hash chain shows internal consistency, an external anchor shows the prefix was not rewritten.',
      'Non è un contatore legale o fiscale (MID/Eichrecht), non è una funzione di sicurezza e non prova chi ha scritto un journal: la catena di hash mostra la coerenza interna, un’ancora esterna mostra che il prefisso non è stato riscritto.'],

    // empty state
    drop_title: ['Drop a journal here', 'Trascina qui un journal'],
    drop_body: [
      'Drop a charger_monitor journal to verify its hash chain, see what may be invoiced and what is held, and open the evidence behind any session. You can also drop an anchor file (charger.anchor.v1) or a captured snapshot stream.',
      'Trascina un journal di charger_monitor per verificarne la catena di hash, vedere cosa si può fatturare e cosa è trattenuto, e aprire le prove dietro ogni sessione. Puoi trascinare anche un file di ancora (charger.anchor.v1) o uno stream di snapshot catturato.'],
    promise1_t: ['Nothing leaves this tab', 'Niente esce da questa scheda'],
    promise1_b: ['The file is read locally. There is no upload and no server component.', 'Il file è letto in locale. Nessun upload, nessuna componente server.'],
    promise2_t: ['Independent verification', 'Verifica indipendente'],
    promise2_b: ['A second implementation of the journal rules, tested byte for byte against the C++ tools.', 'Una seconda implementazione delle regole del journal, testata byte per byte contro gli strumenti C++.'],
    promise3_t: ['Exact money, exact energy', 'Denaro ed energia esatti'],
    promise3_b: ['Integers only (µWh, µEUR). The invoice follows from the meter and the tariff of record, and you can see the arithmetic.', 'Solo interi (µWh, µEUR). La fattura discende dal contatore e dalla tariffa di record, e l’aritmetica è visibile.'],
    promise4_t: ['It cannot touch your plant', 'Non può toccare l’impianto'],
    promise4_b: ['No write path to a charger, a bus or a server. It is a window, not a remote control.', 'Nessun percorso di scrittura verso caricatore, bus o server. È una finestra, non un telecomando.'],
    empty_hint: [
      'Tip: serve this file next to your journals and open it with ?journal=bay.journal to load one automatically. The Live tab polls status files written by charger_monitor --snapshot-file.',
      'Suggerimento: servi questo file accanto ai journal e aprilo con ?journal=bay.journal per caricarne uno in automatico. La scheda Live legge i file di stato scritti da charger_monitor --snapshot-file.'],
    no_demo: ['Demo data is not embedded in this copy.', 'I dati demo non sono incorporati in questa copia.'],
    unknown_json: ['That JSON file is not a journal, anchor or snapshot capture.', 'Quel file JSON non è un journal, un’ancora o una cattura di snapshot.'],
    verifying: ['Verifying chain…', 'Verifica della catena…'],

    // chain banner
    chain_ok: ['Chain verified', 'Catena verificata'],
    chain_ok_body: ['{n} lines recomputed in this browser with SHA-256; every link matches ({b} bytes).', '{n} righe ricalcolate in questo browser con SHA-256; ogni anello coincide ({b} byte).'],
    chain_scope: ['This proves internal consistency, not who wrote the file. Pin it with an anchor to prove the prefix was not rewritten.', 'Questo prova la coerenza interna, non chi ha scritto il file. Fissalo con un’ancora per provare che il prefisso non è stato riscritto.'],
    chain_bad: ['Chain broken at line {n}', 'Catena interrotta alla riga {n}'],
    chain_bad_body: ['A record no longer matches its hash, or a line was removed, inserted or reordered. Do not invoice from this file.', 'Un record non coincide più con il suo hash, oppure una riga è stata rimossa, inserita o riordinata. Non fatturare da questo file.'],
    chain_bad_trunc: ['A line is truncated or malformed. Do not invoice from this file.', 'Una riga è troncata o malformata. Non fatturare da questo file.'],
    chain_prefix: ['Only the verified prefix is shown: lines 1–{n}, {s} closed sessions. Everything after the break is untrusted.', 'È mostrato solo il prefisso verificato: righe 1–{n}, {s} sessioni chiuse. Tutto ciò che segue l’interruzione non è affidabile.'],
    chain_broken: ['Chain broken', 'Catena interrotta'],

    // overview
    hero_k: ['Ready to invoice', 'Pronto da fatturare'],
    hero_d: ['{n} billable sessions · {kwh} kWh · {a} assets', '{n} sessioni fatturabili · {kwh} kWh · {a} asset'],
    excl_vat: ['excl. VAT', 'IVA esclusa'],
    k_held: ['Held for review', 'Trattenuto per revisione'],
    k_energy: ['Energy delivered', 'Energia erogata'], k_energy_d: ['{n} kWh of it in held sessions', 'di cui {n} kWh in sessioni trattenute'],
    k_sessions: ['Sessions closed', 'Sessioni chiuse'],
    k_mean_power: ['Mean power', 'Potenza media'], k_mean_power_d: ['energy ÷ charging time', 'energia ÷ tempo di carica'],
    k_rev_kwh: ['Revenue per kWh', 'Ricavo per kWh'], k_rev_kwh_d: ['all sessions, incl. time and flat fees', 'tutte le sessioni, incl. tempo e quota fissa'],
    k_orphans: ['Orphan sessions', 'Sessioni orfane'], k_orphans_d: ['opened, never closed: not billable', 'aperte, mai chiuse: non fatturabili'],
    k_faults: ['Fault stops', 'Arresti per guasto'], k_faults_d: ['{n} communication timeouts', '{n} timeout di comunicazione'],
    sessions_lc: ['sessions', 'sessioni'], billable_lc: ['billable', 'fatturabili'], held_lc: ['held', 'trattenute'], lines_lc: ['lines', 'righe'],
    closed_lc: ['closed', 'chiuse'], opened_lc: ['opened', 'aperte'], orphans_lc: ['orphans', 'orfane'],
    local_note: ['verified locally', 'verificato in locale'],
    billable: ['Billable', 'Fatturabile'], held: ['Held', 'Trattenuta'],
    c_daily: ['Energy per day', 'Energia al giorno'], c_daily_sub: ['kWh by close date (UTC)', 'kWh per data di chiusura (UTC)'],
    c_bay: ['Value per bay', 'Valore per baia'], c_bay_sub: ['€ excl. VAT', '€ IVA esclusa'], top_n: ['top {n}', 'prime {n}'],
    c_heat: ['When energy is delivered', 'Quando si eroga energia'], c_heat_sub: ['UTC hour × weekday, estimated', 'ora UTC × giorno, stimata'],
    estimated: ['(estimate)', '(stima)'], less: ['less', 'meno'], more: ['more', 'più'],
    c_attention: ['Needs attention', 'Da guardare'], c_attention_sub: ['why money is held', 'perché il denaro è trattenuto'],
    nothing_held: ['Nothing is held: every closed session is billable.', 'Niente trattenuto: ogni sessione chiusa è fatturabile.'],
    largest_held: ['Largest held sessions — click to open the evidence', 'Sessioni trattenute più grandi — clicca per aprire le prove'],
    orphans_note: ['{n} session(s) opened and never closed (the process died mid-session). Their energy is unknown and not billable.', '{n} sessione/i aperte e mai chiuse (il processo è morto a sessione in corso). La loro energia è ignota e non fatturabile.'],
    table_view: ['Table', 'Tabella'], chart_view: ['Chart', 'Grafico'],
    opened_at: ['Opened at', 'Aperta il'], quality_aria: ['Share of billable and held sessions', 'Quota di sessioni fatturabili e trattenute'],
    day: ['Day', 'Giorno'], hour_utc: ['Hour (UTC)', 'Ora (UTC)'],

    // columns
    asset: ['Asset', 'Asset'], sessions: ['Sessions', 'Sessioni'], status: ['Status', 'Stato'], reason: ['Reason', 'Motivo'],
    col_asset: ['Asset', 'Asset'], col_id: ['Session', 'Sessione'], col_closed: ['Closed (UTC)', 'Chiusa (UTC)'], col_status: ['Status', 'Stato'],
    col_energy: ['Energy', 'Energia'], col_duration: ['Duration', 'Durata'], col_stop: ['Stop', 'Arresto'], col_total: ['Total €', 'Totale €'], col_line: ['Line', 'Riga'],
    col_sessions: ['Sessions', 'Sessioni'], col_billable: ['Billable', 'Fatturabili'], col_held: ['Held', 'Trattenute'], col_held_share: ['Held %', '% trattenute'],
    col_revenue: ['Value €', 'Valore €'], col_charging_time: ['Charging time', 'Tempo di carica'], col_mean_power: ['Mean power', 'Potenza media'],
    col_fault_stops: ['Fault stops', 'Arresti guasto'], col_timeout_stops: ['Timeouts', 'Timeout'], col_undersampled: ['Undersampled', 'Campioni scarsi'], col_daily_trend: ['Daily energy', 'Energia/giorno'],
    fleet_note: [
      'One row per asset, rebuilt from the journal alone. Availability is not shown: sessions alone carry no in-service time (the live monitor reports it). Click a row to see its sessions.',
      'Una riga per asset, ricostruita dal solo journal. La disponibilità non è mostrata: le sole sessioni non portano il tempo in servizio (lo riporta il monitor live). Clicca una riga per vedere le sue sessioni.'],

    // sessions
    search: ['Search asset, id, reason…', 'Cerca asset, id, motivo…'], all_assets: ['All assets', 'Tutti gli asset'], all_status: ['Billable + held', 'Fatturabili + trattenute'],
    all_reasons: ['All reasons', 'Tutti i motivi'], from: ['From', 'Dal'], to: ['To', 'Al'], clear: ['Clear', 'Azzera'], export_csv: ['Settlement CSV', 'CSV di liquidazione'],
    no_match: ['No session matches these filters.', 'Nessuna sessione corrisponde ai filtri.'], page: ['page', 'pagina'],
    csv_note: [
      'The CSV has the same columns as charger_settle. Text cells that a spreadsheet would run as a formula are neutralised with a leading apostrophe.',
      'Il CSV ha le stesse colonne di charger_settle. Le celle di testo che un foglio di calcolo eseguirebbe come formula sono neutralizzate con un apice iniziale.'],
    reason_NO_INTERVAL: ['Too few samples', 'Campioni insufficienti'], reason_GAPS: ['Measurement gap', 'Buco di misura'], reason_ZERO_ENERGY: ['Zero energy', 'Energia zero'],
    reason_COMMUNICATION_TIMEOUT: ['Communication timeout', 'Timeout di comunicazione'], reason_PRICE_OVERFLOW: ['Price overflow', 'Overflow di prezzo'], reason_UNSPECIFIED: ['Unspecified', 'Non specificato'],
    stop_STATUS_OFF: ['Switched off', 'Spento'], stop_STATUS_READY: ['Back to ready', 'Tornato pronto'], stop_COMMUNICATION_TIMEOUT: ['Communication timeout', 'Timeout di comunicazione'],
    stop_POWER_STAGE_FAULT: ['Power stage fault', 'Guasto stadio di potenza'], stop_MEASUREMENT_OUT_OF_RANGE: ['Measurement out of range', 'Misura fuori range'],
    stop_MONITOR_SHUTDOWN: ['Monitor shut down', 'Monitor spento'],
    stop_ENERGY_COUNTER_SATURATED: ['Energy counter saturated', 'Contatore saturo'],

    // drawer
    close: ['Close', 'Chiudi'],
    why_billable: ['Clean close, at least two samples, no gap and no timeout: this is the only kind of session an automated invoice may trust.', 'Chiusura pulita, almeno due campioni, nessun buco e nessun timeout: è l’unico tipo di sessione di cui una fatturazione automatica può fidarsi.'],
    why_NO_INTERVAL: ['Fewer than two samples: no energy interval was measured, so the energy is a lower bound.', 'Meno di due campioni: nessun intervallo di energia è stato misurato, quindi l’energia è un limite inferiore.'],
    why_GAPS: ['A sample interval exceeded the integration step: no energy was invented for the silent time, so the energy is a lower bound.', 'Un intervallo tra campioni ha superato il passo di integrazione: nessuna energia è stata inventata per il tempo di silenzio, quindi l’energia è un limite inferiore.'],
    why_ZERO_ENERGY: ['The session integrated to zero energy.', 'La sessione ha integrato energia zero.'],
    why_COMMUNICATION_TIMEOUT: ['The link was lost: the billed time is an upper bound and the energy a lower bound.', 'Il collegamento è caduto: il tempo fatturato è un limite superiore e l’energia un limite inferiore.'],
    why_PRICE_OVERFLOW: ['The price did not fit in 64 bits; it was held rather than recorded as free energy.', 'Il prezzo non entrava in 64 bit; è stata trattenuta invece di registrare energia gratis.'],
    why_INTERRUPTED: ['The monitor was stopped while the session was open (an update or a restart). The energy accepted until then is exact but a lower bound; the session is held for review, and the charge may continue as the next session.', 'Il monitor è stato fermato a sessione aperta (aggiornamento o riavvio). L’energia accettata fino a quel momento è esatta ma è un limite inferiore; la sessione è trattenuta per revisione e la carica può proseguire come sessione successiva.'],
    why_UNSPECIFIED: ['Held for a reason this dashboard does not recognise.', 'Trattenuta per un motivo che questa dashboard non riconosce.'],
    d_meter: ['Meter values', 'Valori di misura'], d_cost: ['Cost', 'Costo'], per_session: ['per session', 'a sessione'], total: ['Total', 'Totale'],
    clamped: ['clamped by the tariff of record', 'limitato dalla tariffa di record'],
    d_recompute: ['Independent recalculation', 'Ricalcolo indipendente'], field: ['Field', 'Campo'], in_journal: ['Journal', 'Journal'], recomputed: ['Recomputed', 'Ricalcolato'],
    recompute_ok: ['Every derived number follows from the meter registers and the tariff of record.', 'Ogni numero derivato discende dai registri di misura e dalla tariffa di record.'],
    recompute_bad: ['At least one number does not follow from the meter and the tariff of record. Treat the record as suspect.', 'Almeno un numero non discende dal contatore e dalla tariffa di record. Considera sospetto il record.'],
    recompute_scope: ['This checks the arithmetic against the tariff stored in the same line. It cannot tell whether that tariff was the contracted one.', 'Verifica l’aritmetica rispetto alla tariffa memorizzata nella stessa riga. Non può dire se quella tariffa fosse quella contrattuale.'],
    d_chain: ['Chain provenance', 'Provenienza nella catena'], open_line: ['Open line', 'Riga di apertura'], close_line: ['Close line', 'Riga di chiusura'],
    d_chain_note: ['The close hash is SHA-256(prev_hash TAB rest-of-line); the next line carries it as its prev_hash.', 'L’hash di chiusura è SHA-256(prev_hash TAB resto-riga); la riga successiva lo porta come prev_hash.'],
    d_verify: ['Verify without this page', 'Verifica senza questa pagina'], d_verify_note: ['Standard tools only — no charger_monitor binary needed:', 'Solo strumenti standard — nessun binario charger_monitor:'],
    d_cli: ['With the C++ tools, the full evidence pack:', 'Con gli strumenti C++, il pacchetto di prove completo:'], d_raw: ['Raw close line', 'Riga di chiusura grezza'],
    copy: ['Copy', 'Copia'], copied: ['Copied', 'Copiato'], closed_at: ['Closed at', 'Chiusa il'], yes: ['yes', 'sì'], no: ['no', 'no'],

    // integrity
    i_chain: ['Hash chain', 'Catena di hash'], i_chain_sub: ['one cell per group of lines', 'una cella per gruppo di righe'],
    strip_aria: ['Verification status along the journal', 'Stato di verifica lungo il journal'],
    verified: ['Verified', 'Verificato'], first_bad: ['First bad line', 'Prima riga errata'], unverified: ['Not trusted (after the break)', 'Non affidabile (dopo l’interruzione)'],
    file: ['File', 'File'], size: ['Size', 'Dimensione'], whole_file: ['whole file', 'file intero'], lines: ['Lines', 'Righe'], chained: ['chained', 'concatenate'], raw: ['raw', 'grezze'], line: ['line', 'riga'],
    forensics: ['What happened', 'Cosa è successo'],
    forensics_tail: ['{ok} of the {n} lines after the break are internally consistent on their own — evidence about the shape of the damage, not a reason to trust them.', '{ok} delle {n} righe dopo l’interruzione sono internamente coerenti da sole — un indizio sulla forma del danno, non un motivo per fidarsene.'],
    forensics_scope: ['The trust boundary stays at the first bad line, exactly like charger_monitor.', 'Il confine di fiducia resta alla prima riga errata, esattamente come in charger_monitor.'],
    orphans: ['Orphan sessions', 'Sessioni orfane'], orphans_sub: ['opened, never closed', 'aperte, mai chiuse'],
    no_orphans: ['No orphan sessions.', 'Nessuna sessione orfana.'],
    anchor: ['External anchor', 'Ancora esterna'], anchor_sub: ['charger.anchor.v1', 'charger.anchor.v1'],
    anchor_body: [
      'An anchor pins the journal prefix (digest, last hash, line count) into a small file you keep on append-only storage. Verifying it later proves the prefix was not rewritten, even by someone who recomputed the whole chain — while the journal may keep growing.',
      'Un’ancora fissa il prefisso del journal (digest, ultimo hash, numero di righe) in un piccolo file da custodire su storage append-only. Verificarla in seguito prova che il prefisso non è stato riscritto, anche da chi ha ricalcolato l’intera catena — mentre il journal può continuare a crescere.'],
    anchor_create: ['Create anchor', 'Crea ancora'], anchor_verify: ['Verify against an anchor…', 'Verifica con un’ancora…'], anchor_preview: ['Preview the anchor document', 'Anteprima del documento di ancora'],
    anchor_ok: ['Anchor verified', 'Ancora verificata'], anchor_ok_body: ['Line {n} still hashes to the anchored value; {a} lines were appended since.', 'La riga {n} produce ancora il valore ancorato; da allora sono state aggiunte {a} righe.'],
    anchor_same: ['The whole-file digest equals the anchored one: the journal has not changed at all.', 'Il digest dell’intero file coincide con quello ancorato: il journal non è cambiato affatto.'],
    anchor_grown: ['The journal grew after the anchor; the chain proves the new lines link to the anchored prefix.', 'Il journal è cresciuto dopo l’ancora; la catena prova che le nuove righe si agganciano al prefisso ancorato.'],
    anchor_bad: ['Anchor does NOT verify', 'L’ancora NON verifica'],
    anchor_store: ['Keep anchors on WORM / object-lock storage or a printed digest. An anchor is exact-state evidence, not a signature.', 'Conserva le ancore su storage WORM / object-lock o come digest stampato. Un’ancora è una prova di stato esatto, non una firma.'],
    cmp_title: ['Compare with an earlier copy', 'Confronta con una copia precedente'], cmp_sub: ['no anchor needed', 'nessuna ancora necessaria'],
    cmp_body: [
      'Two copies of a journal taken at different times are each other’s witness: if the earlier one is an exact prefix of this one, the history up to that point was not rewritten, with no anchor at all.',
      'Due copie di un journal prese in momenti diversi si fanno da testimoni: se la precedente è un prefisso esatto di questa, la storia fino a quel punto non è stata riscritta, anche senza ancora.'],
    cmp_pick: ['Pick the earlier copy…', 'Scegli la copia precedente…'],
    cmp_identical: ['Identical copies', 'Copie identiche'], cmp_identical_body: ['Both chains verify and every one of the {e} lines is the same.', 'Entrambe le catene verificano e tutte le {e} righe coincidono.'],
    cmp_prefix: ['The earlier copy is an exact prefix', 'La copia precedente è un prefisso esatto'],
    cmp_prefix_body: ['All {e} lines of the earlier copy are unchanged here; {a} lines were appended since.', 'Tutte le {e} righe della copia precedente sono invariate qui; da allora sono state aggiunte {a} righe.'],
    cmp_diverged: ['The histories DIVERGE', 'Le storie DIVERGONO'],
    cmp_diverged_body: ['They agree on the first {k} lines and differ from line {n} on: the earlier history was rewritten, or the two files are not copies of the same journal.', 'Coincidono nelle prime {k} righe e differiscono dalla riga {n}: la storia precedente è stata riscritta, oppure i due file non sono copie dello stesso journal.'],
    cmp_shorter: ['The earlier copy is longer than this journal: lines were removed.', 'La copia precedente è più lunga di questo journal: sono state rimosse righe.'],
    cmp_diverged_note: ['Both chains verify on their own, so each is internally consistent; that is exactly the case an attacker who recomputes the chain produces.', 'Entrambe le catene verificano da sole, quindi ciascuna è coerente al proprio interno; è proprio il caso prodotto da chi ricalcola la catena.'],
    cmp_broken: ['Cannot compare: a chain does not verify', 'Impossibile confrontare: una catena non verifica'],
    cmp_broken_earlier: ['The earlier copy is broken at line {n}.', 'La copia precedente è interrotta alla riga {n}.'], cmp_broken_current: ['This journal is broken at line {n}.', 'Questo journal è interrotto alla riga {n}.'],
    proof: ['What a verified chain does and does not prove', 'Cosa prova e cosa non prova una catena verificata'], proves: ['It proves', 'Prova'], not_proves: ['It does not prove', 'Non prova'],
    proof_p1: ['No line was edited, inserted, removed or reordered without breaking a link.', 'Nessuna riga è stata modificata, inserita, rimossa o riordinata senza rompere un anello.'],
    proof_p2: ['Every record is exactly as the writer appended it, in order.', 'Ogni record è esattamente come lo scrittore lo ha accodato, in ordine.'],
    proof_p3: ['With an anchor: the prefix up to the anchored line is unchanged since the anchor was made.', 'Con un’ancora: il prefisso fino alla riga ancorata è invariato da quando l’ancora è stata creata.'],
    proof_n1: ['Who wrote the file. A full rewrite from the genesis block recomputes cleanly.', 'Chi ha scritto il file. Una riscrittura completa dal blocco genesi si ricalcola correttamente.'],
    proof_n2: ['That the meter was right: the figures are an operational record, not a legal measurement.', 'Che il contatore fosse corretto: le cifre sono un registro operativo, non una misura legale.'],
    proof_n3: ['That the tariff of record was the contracted one.', 'Che la tariffa di record fosse quella contrattuale.'],
    verify_alone: ['Check line 1 and the whole file with standard tools:', 'Controlla la riga 1 e il file intero con strumenti standard:'],

    // live
    live_title: ['Live status', 'Stato live'], live_sub: ['polls same-origin snapshot files', 'legge file di snapshot della stessa origine'],
    live_howto: [
      'Run charger_monitor with --snapshot-file status/bay-01.json, serve this page and that folder from the same web server, then list the files below (one per line). The monitor rewrites the file atomically; this page only reads it.',
      'Avvia charger_monitor con --snapshot-file status/bay-01.json, servi questa pagina e quella cartella dallo stesso web server, poi elenca i file qui sotto (uno per riga). Il monitor riscrive il file in modo atomico; questa pagina lo legge soltanto.'],
    live_sources: ['Status files', 'File di stato'], live_start: ['Start', 'Avvia'], live_stop: ['Stop', 'Ferma'], live_every: ['Poll every', 'Leggi ogni'],
    live_none: ['No sources yet. Add a status file path above and press Start, or load a captured stream.', 'Nessuna sorgente. Aggiungi un percorso di file di stato qui sopra e premi Avvia, oppure carica uno stream catturato.'],
    live_wait: ['Waiting for the first snapshot…', 'In attesa del primo snapshot…'], live_err: ['Cannot read this status file', 'Impossibile leggere questo file di stato'],
    live_file_note: ['This page was opened from a file: browsers block reading other files from here. Serve the folder over HTTP (for example python3 -m http.server) to use polling. Captured streams still work.', 'Questa pagina è stata aperta da file: i browser bloccano la lettura di altri file da qui. Servi la cartella via HTTP (ad esempio python3 -m http.server) per usare il polling. Gli stream catturati funzionano comunque.'],
    not_snapshot: ['not a charger.snapshot.v1 document', 'non è un documento charger.snapshot.v1'],
    capture_load: ['Load captured stream…', 'Carica stream catturato…'], capture_demo: ['Demo capture (synthetic day)', 'Cattura demo (giornata sintetica)'],
    capture_title: ['Captured stream', 'Stream catturato'], capture_close: ['Back to live', 'Torna al live'], capture_empty: ['No snapshots found in that file.', 'Nessuno snapshot trovato in quel file.'],
    capture_unit: ['line: power in kW · strip: machine state', 'linea: potenza in kW · striscia: stato macchina'], capture_pos: ['Position in the capture', 'Posizione nella cattura'], capture_chart: ['Power over the capture, with machine state', 'Potenza durante la cattura, con stato macchina'], speed: ['Speed', 'Velocità'],
    capture_note: ['A capture is the stdout of charger_monitor (one JSON line per frame) or a series of status files. Replaying it shows what an operator would have seen.', 'Una cattura è lo stdout di charger_monitor (una riga JSON per frame) o una serie di file di stato. Riprodurla mostra cosa avrebbe visto un operatore.'],
    state_CHARGING: ['Charging', 'In carica'], state_READY: ['Ready', 'Pronto'], state_OFF: ['Off', 'Spento'], state_FAULT: ['Fault', 'Guasto'], state_DISCONNECTED: ['Disconnected', 'Disconnesso'], state_UNKNOWN: ['Unknown', 'Sconosciuto'],
    voltage: ['Voltage', 'Tensione'], current: ['Current', 'Corrente'], power: ['Power', 'Potenza'],
    file_fresh: ['updated {s} s ago', 'aggiornato {s} s fa'], file_stale: ['status file not updated for {s} s', 'file di stato non aggiornato da {s} s'],
    action_reported: ['Action reported by the monitor:', 'Azione riportata dal monitor:'],
    meas_stale: ['Measurements stale', 'Misure non aggiornate'], status_stale: ['Status stale', 'Stato non aggiornato'], no_advisories: ['No active advisories', 'Nessun avviso attivo'],
    diag_stuck: ['Measurement stuck', 'Misura bloccata'], diag_vrate: ['Voltage slope', 'Pendenza tensione'], diag_irate: ['Current slope', 'Pendenza corrente'], diag_sag: ['Voltage sag under load', 'Caduta di tensione sotto carico'], diag_hunt: ['Current hunting', 'Oscillazione di corrente'],
    session_open: ['Session in progress', 'Sessione in corso'], avail: ['Availability', 'Disponibilità'], util: ['Utilization', 'Utilizzo'], frames_ok: ['Frames accepted', 'Frame accettati'], frames_rej: ['Rejected', 'Scartati'],
    timeouts: ['Timeouts', 'Timeout'], meas_age: ['Measurement age', 'Età misura'],

    // report
    print_evidence: ['Print evidence', 'Stampa le prove'],
    save_report: ['Save report', 'Salva report'], print: ['Print', 'Stampa'],
    report_hint: ['Download a single HTML file with this journal embedded: it re-verifies itself when opened.', 'Scarica un unico file HTML con questo journal incorporato: si riverifica da solo all’apertura.'],
    report_big: ['This journal is too large to embed (24 MB limit).', 'Questo journal è troppo grande per essere incorporato (limite 24 MB).']
  };

  function t(lang, key) {
    var e = L[key];
    if (!e) return key;
    return (lang === 'it' ? e[1] : e[0]) || e[0];
  }
  root.ChargerI18n = { t: t, table: L };
})(typeof self !== 'undefined' ? self : this);
