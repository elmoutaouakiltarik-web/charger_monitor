# Guida di campo — messa in servizio, collaudo e dispute

Per ingegneri e tecnici che installano il monitor su una baia reale.
Niente numeri inventati: ogni cifra che fattura esce dal vostro journal,
ogni cifra di targa esce dai vostri strumenti. Questa guida dice **come
misurare**, non **quanto vale**.

## 1. Confini — leggeteli prima di avvitare qualsiasi cosa

- Non è uno strumento di misura legale: non è un contatore MID (Direttiva
  2014/32/EU, allegato MI-003; in Italia D.Lgs. 22/2007 e DM 93/2017) né un
  sistema di misura per EVSE, e non ha nulla a che vedere con l'Eichrecht
  (MessEG/MessEV), che è diritto **tedesco**. Non è una funzione di sicurezza
  (IEC 61508, ISO 13849) e non comanda il power stage. Produce il record locale
  aritmeticamente esatto; la certificazione è un'altra azienda, la sicurezza resta
  negli interlock. Dettagli e fonti: [COMPLIANCE.md](COMPLIANCE.md).
- "Esatto" significa **aritmeticamente esatto** (nessuna deriva di arrotondamento).
  Non significa *accurato*: l'accuratezza è limitata dai sensori di tensione e
  corrente dietro i segnali CAN, che qui nessuno ha valutato.
- Il journal dimostra **coerenza interna** (nessuna riga modificata senza
  rompere la catena). Non dimostra provenienza: chi riscrive l'intero file
  ricalcola la catena. Se il rischio dispute lo richiede, ancorate (§6).
- `billing_complete == true` è l'unico campo che la fatturazione automatica
  può leggere. Tutto il resto resta in revisione manuale. Mai fatturare
  `energy_lower_bound` / `time_upper_bound` senza revisione.

## 2. Messa in servizio (checklist)

1. **Bus**: verificate bitrate e terminazione con `candump`/`cansend`
   (tipicamente 500 kbit/s su impianti industriali — confermate sul vostro
   schema, non su questa riga). Mettete l'interfaccia in **listen-only**
   (`deploy/systemd/80-can0.network`): è la versione hardware di "non trasmette
   mai", il controller non può nemmeno dare l'ACK.
2. **DBC del costruttore**: adottate il file OEM, non l'ID demo.
   ```bash
   ./build/charger_monitor --dump-dbc --dbc vendor.dbc
   ./build/charger_monitor --replay --dbc vendor.dbc \
       --voltage-signal DC_Voltage --current-signal DC_Current < frames.txt
   ```
   Regole fatte rispettare all'avvio: segnali di misura a 16 bit unsigned,
   zero-offset, allineati al byte; fattore come scala razionale esatta;
   stato da tabella `VAL_` con almeno due significati distinti. Qualsiasi
   cosa diversa abortisce l'avvio con il motivo — un decode indovinato
   fatturerebbe numeri sbagliati in silenzio.
3. **Configurazione**: generate il template, riempitelo, validatelo.
   ```bash
   ./build/charger_monitor --print-config > bay-07.json
   # editare: asset_id, timeout, tariffa, diagnostica
   ```
   Un campo errato o sconosciuto blocca l'avvio con il percorso del campo:
   una tariffa digitata male non resta mai a zero in silenzio. Verificate
   esplicitamente che `energy_eur_per_kwh` sia la tariffa contrattualizzata:
   **tariffa zero = fattura zero che nessuno nota**.
4. **Soglie diagnostiche**: tutte disabilitate di default. Impostatele dai
   vostri dati elettrici (stuck-at, slew rate V/A, sag sotto carico, hunting)
   con l'ingegnere elettrico. Una diagnostica non blocca mai la carica:
   solleva un warning, incrementa un contatore, appare in
   `snapshot.diagnostics`.
5. **Storage del journal**: filesystem durevole, `fsync` attivo di default.
   Mai `/tmp`, mai NFS senza commit sincrono. Il journal è il registro di
   fatturazione: trattatelo come tale (permessi, backup, rotazione). Il suo
   orologio è quello della macchina: **NTP attivo**, perché il timestamp di
   chiusura è un'etichetta di orologio a muro.
6. **Servizio**: usate l'unit `deploy/systemd/charger-monitor@.service`
   (sandbox, `stdout` scartato, journal in `/var/lib/charger`, stato su tmpfs).
   Dettagli in [OPERATIONS.md](OPERATIONS.md).
7. **Supervisione**: `--snapshot-file` per la dashboard e `--metrics-file` per
   Prometheus. Il monitor non apre nessuna porta in ascolto.

## 3. Collaudo di accettazione (dopo ogni installazione e ogni aggiornamento)

```bash
# 1. Replay di una sequenza nota e verifica della catena
./build/charger_monitor --replay --journal bay.journal --replay-interval-ms 1000 < frames.txt
./build/charger_monitor --verify-journal --journal bay.journal   # exit 0 integro, 3 alterato

# 2. Quadratura economica: solo billing_complete si fattura
./build/charger_settle bay.journal --summary

# 3. Prova di una fattura contestata (pacchetto completo in un JSON)
./build/charger_settle bay.journal --dispute bay-07 12

# 4. Record per il backend (concetti OCPI, VAT omesso, start derivato e dichiarato)
./build/charger_settle bay.journal --cdr bay-07 12

# 5. Ancora esterna su storage append-only (WORM, bucket object-lock, o stampa archiviata)
./build/charger_monitor --anchor-journal --journal bay.journal --anchor-file /mnt/worm/bay-07.anchor
./build/charger_monitor --verify-anchor --journal bay.journal --anchor-file /mnt/worm/bay-07.anchor

# 6. Vettore canonico: 480 V x 10 A x 1 h = esattamente 4800 Wh (su qualsiasi macchina)
./build/charger_bench        # righe EXACT riproducibili; il throughput è misurato, non promesso

# 7. Seconda implementazione indipendente: aprite il journal nella dashboard
#    (dashboard/charger-control-room.html): catena, totali e ricalcolo del prezzo
#    devono coincidere con i punti 1-3.
```

Il collaudo passa se e solo se: catena integra, `billing_complete` solo su
sessioni pulite, dispute pack verificabile con strumenti standard (§5),
ancora verificata, bench senza DRIFT sulle righe exact, dashboard concorde.

## 4. Tariffe orarie (TOU) — per spostare i carichi di notte

```bash
./build/charger_monitor --print-tou > tou.json   # template charger.tou.v1
./build/charger_settle bay.journal --check-tou tou.json        # valida copertura 24 h
./build/charger_settle bay.journal --tou-settle tou.json --summary
```

- Ore in `HH:MM` **UTC**; `end < start` attraversa la mezzanotte.
- Ogni secondo del giorno deve essere coperto da una finestra o dal fallback:
  un buco viene **rifiutato** (exit 2), mai prezzato a zero. Zero è la
  direzione peggiore in cui sbagliare su un canale di fatturazione.
- La colonna `tou_estimate_eur` è una **stima a potenza costante** per
  decidere lo shifting notturno, non la fattura: il journal non conserva la
  serie temporale intra-sessione. Il totale esatto del ledger resta l'unico
  record fatturabile. Sessioni con `closed_at_utc` non interpretabile sono
  elencate come `refused`, non stimate.
- Le tariffe nei file `charger_project/examples/tou/` sono **segnaposto** per
  esercitare il percorso: sostituitele con la tariffa contrattualizzata del
  fornitore prima di qualsiasi analisi.
- La heat map oraria della dashboard usa la stessa assunzione (potenza costante)
  e la dichiara come stima.

## 5. Procedura di dispute — rispondere a "perché 5,30 EUR?"

1. Aprite il journal nella dashboard (scheda **Sessioni** → riga → pannello prove)
   oppure riesportate il pacchetto: `charger_settle bay.journal --dispute ASSET ID`.
   Contiene contatori, tariffa di record, calcolo e provenienza della catena
   (`open`/`close`, `close_hash`, digest del file). La dashboard ricalcola da sola,
   in modo indipendente, ogni numero derivato dai registri e dalla tariffa di record.
2. Riverificate senza il nostro software (consegnate queste righe al cliente):
   ```bash
   sha256sum bay.journal
   sed -n '<close_line>p' bay.journal | cut -f1,3- | tr -d '\n' | sha256sum
   sed -n '<close_line>p' bay.journal | cut -f2
   ```
   Il secondo digest deve eguagliare il terzo e il `close_hash` del pacchetto: la
   catena è `SHA256(prev_hash TAB resto)`, ricalcolabile con `sed`, `cut` e
   `sha256sum`. **Attenzione al `tr -d '\n'`**: la riga estratta termina con un
   a-capo che non fa parte dell'input dell'hash; senza di esso il digest sarà
   diverso e sembrerà che il journal sia alterato quando non lo è.
3. Ricalcolate il prezzo dal blocco tariffario incorporato
   (`energy_cost + time_cost + flat_cost == total_cost`, interi in micro-EUR;
   salvo il clamp min/max dichiarato nella stessa tariffa).
4. Se esiste un'ancora (§6), verificate il prefisso: una riscrittura totale
   del file non sopravvive all'ancora anche se la catena interna torna.

## 6. Ancora esterna e data certa

La catena vive nello stesso file che protegge. L'ancora ne estrae lo stato
(`sha256` del file, `last_hash`, numero righe, timestamp UTC) in un file
separato da custodire su storage append-only:

- Scrivete l'ancora a fine turno / fine giornata dal sistema di supervisione.
- Conservate le ancore in ordine: ognuna impegna il prefisso fino alla sua riga.
- `verify-anchor` dimostra che il prefisso non è stato riscritto **e** lascia
  crescere il journal: le righe aggiunte dopo l'ancora sono riportate
  (`appended`), non rifiutate — la catena prova il suffisso, l'ancora il prefisso.
- L'ancora non è una firma: non attribuisce chi ha scritto. Se serve
  attribuzione (firma operatore / PKI di sito), firmate il file ancora con la
  vostra infrastruttura esistente — il formato è JSON stabile e testo.
- La dashboard crea e verifica ancore compatibili byte per byte con gli strumenti C++.

### 6.1 Data certa dell'ancora (RFC 3161)

L'ancora prova **cosa** conteneva il prefisso, non **quando** l'avete fatta:
`anchored_at_utc` è l'orologio locale che dichiara se stesso. Per una data
certa, marcate temporalmente l'ancora con una Time-Stamp Authority. Esce dal
sito solo un digest SHA-256 da 32 byte.

```bash
scripts/notarise.sh request  bay-07.anchor                          # -> bay-07.anchor.tsq
scripts/notarise.sh submit   bay-07.anchor https://TSA.EXAMPLE/tsr  # -> bay-07.anchor.tsr
scripts/notarise.sh inspect  bay-07.anchor                          # stato, genTime, serial, nonce
scripts/notarise.sh verify   bay-07.anchor tsa-root.pem [tsa-chain.pem]
```

Lo script usa solo `openssl` (e `curl`). `request`, `inspect` e `verify` sono
stati provati contro una TSA di prova locale: un'ancora modificata dà
`message imprint mismatch` ed exit non zero. Il passo `submit` verso una TSA
reale **non è stato provato** nell'ambiente di sviluppo: provatelo sulla vostra.

Limiti, da dire al cliente e all'auditor senza giri di parole:

- Una marca temporale prova che quel digest esisteva **prima** di `genTime`. Non
  prova che il contenuto fosse vero, né chi l'ha scritto, né che nulla fosse
  stato alterato prima dell'ancora.
- L'ancora è fresca quanto la sua frequenza: le righe scritte dopo l'ultima
  ancora marcata si possono riscrivere senza rilevarlo fino alla successiva.
- Conta solo il `genTime` della TSA, non `anchored_at_utc`.
- I certificati TSA scadono. Conservate il `.tsr`, la catena e (se potete) i dati
  di revoca accanto all'ancora; la verifica anni dopo va fatta "alla data" del
  `genTime` (lo script lo fa con `-attime`).
- Non è una firma e non è non-ripudio.
- Una marca temporale **qualificata** eIDAS (Reg. 910/2014, art. 41) gode della
  presunzione di accuratezza di data e integrità dei dati; una non qualificata non
  è privata di effetto giuridico solo perché elettronica, ma non gode di quella
  presunzione. Una TSA gratuita non è qualificata.

Cadenza consigliata: ancora a fine turno, marca temporale sull'ancora, `.anchor`
e `.tsr` insieme su WORM.

## 7. Osservabilità — Prometheus e dashboard

```bash
charger_monitor can0 --config bay-07.json --journal /var/lib/charger/bay-07.journal \
    --snapshot-file /run/charger/status/bay-07.json \
    --metrics-file  /var/lib/node_exporter/textfile/charger-bay-07.prom
charger_settle bay-07.journal --metrics --out /var/lib/node_exporter/textfile/charger-fleet.prom
```

- Il monitor riscrive i file in modo **atomico** (scrive `*.tmp` nella stessa
  cartella e fa `rename`): un lettore vede il vecchio file o il nuovo, mai uno
  parziale. Usate tmpfs per `--snapshot-file`: un file riscritto ogni secondo non
  deve consumare flash.
- node_exporter legge solo `*.prom`, rifiuta file con timestamp lato client e
  **non scade mai un file vecchio**: se il monitor muore, serve per sempre gli
  ultimi valori. Per questo c'è `charger_export_timestamp_seconds` e l'allarme
  `ChargerMonitorNotWriting` in `deploy/prometheus/charger-alerts.yml`.
- Un valore che il monitor non può conoscere è **omesso**, mai esportato come
  zero (disponibilità indefinita ≠ 0 %).
- Interi: Prometheus usa float64, esatto fino a 2^53. Un contatore di energia in
  Wh con sei decimali resta esatto fino a circa 9 GWh per serie; oltre, l'arrotondamento
  avviene all'ingestione. Il journal resta la fonte di verità.
- Le soglie e i `for:` nelle regole d'esempio sono **segnaposto**: prendeteli dal
  vostro SLA, come per la diagnostica.

La dashboard (`dashboard/charger-control-room.html`) legge gli stessi file di
stato: scheda **Live**, stato, V/I/P, avvisi, sessione in corso e — soprattutto —
l'età del file di stato. Dettagli in [DASHBOARD.md](DASHBOARD.md).

## 8. Economia — le formule, con i vostri numeri

Nessuna tariffa è scritta in questa guida: i vostri prezzi stanno nel file di
configurazione e nel blocco tariffario di ogni riga di chiusura. Le formule:

- **Ricavo fatturabile** (per sessione `billing_complete`):
  `ceil(energia_Wh, step) × prezzo_energia + ceil(tempo_s, step) × prezzo_tempo + flat`,
  con arrotondamento OCPI a multiplo superiore e clamp min/max opzionali.
  Il report `charger_settle --summary` lo scompone in ENERGY / TIME / FLAT.
- **Perdita da tariffa zero**: `energia_kWh × prezzo_kWh` per ogni sessione
  fatturata a tariffa non configurata. Il guardrail è l'avvio che rifiuta
  configurazioni ambigue — verificate comunque la tariffa a ogni deploy.
- **Costo di una dispute**: `ore_tecnico × costo_orario + accrediti_riconosciuti
  + rischio_churn`. Il pacchetto `--dispute` e il pannello prove della dashboard
  spostano quasi tutto il primo termine da "ore" a "minuti": misurate le vostre
  ore prima e dopo, quella è la metrica che giustifica il progetto.
- **Disponibilità / utilizzo**: dal monitor live (`availability_ratio`,
  `utilization_ratio`) o dal portfolio (`--portfolio`), mai zero fittizio:
  indefinito è `-1` nel JSON e *assente* in Prometheus. Confrontate baie tra loro
  sullo stesso journal, non su finestre diverse.
- **Throughput**: misuratelo in loco con `charger_bench` in Release. Riferimento
  indicativo su macchina di sviluppo: ~1,1–1,5 M frame/s contro ~3 500 frame/s
  di un bus CAN a 500 kbit/s e 1 Hz — l'esattezza è il vincolo, non la velocità.
  Citare il rapporto, non la cifra.

## 9. Errori visti sul campo (e come evitarli)

| Sintomo | Causa probabile | Azione |
| --- | --- | --- |
| Fatture a zero | Tariffa non impostata / config non caricata | Avvio con `--config` versionato, check del blocco tariffario nel dispute pack |
| Sessioni sempre `held` | Timeout troppo stretti o bus disturbato | Allargate `measurement_timeout_ms`/`status_timeout_ms` dai dati reali; bonifica EMC |
| `MEASUREMENT_GAP` frequenti | Buchi di comunicazione | Mai interpolare: la sessione è `lower_bound` e va in revisione |
| Orfani dopo il riavvio | Processo morto a sessione aperta | Energia non fatturabile per costruzione; procedura di revisione manuale. Aggiornate a baia ferma |
| TOU rifiutato | Buco di copertura o finestre sovrapposte | `--check-tou`, aggiungere fallback o correggere gli orari |
| Ancora non verifica | Prefisso riscritto o journal troncato | Bloccare la fatturazione, indagare, non rigenerare l'ancora sul file alterato |
| Dashboard: "file di stato non aggiornato" | Il monitor è fermo o l'orologio del browser è sfasato | Controllate l'unit systemd; confrontate gli orologi (NTP) |
| Prometheus ferma ma nessun allarme | node_exporter serve l'ultimo file per sempre | Allarme su `charger_export_timestamp_seconds` |
| Il digest del paragrafo 5 non torna | Manca `tr -d '\n'` nella ricetta | Usate la ricetta di §5 |

## 10. Manutenzione

- Rieseguite il collaudo (§3) dopo ogni aggiornamento software e ogni cambio
  configurazione. La CI di progetto è configurata ma può essere disattivata
  sull'account: replicate in locale almeno `ctest`, `e2e_replay.sh`, `node --test
  dashboard/tests` e `charger_demo` prima di toccare l'hardware.
- **Aggiornate e ruotate a baia ferma.** Un riavvio con una sessione aperta lascia
  un orfano (energia ignota, non fatturabile) a meno che la chiusura pulita non la
  registri come trattenuta: vedete [OPERATIONS.md](OPERATIONS.md).
- Ruotate i journal per periodo (mensile) e ancorate la chiusura di ogni
  periodo. Un journal da un anno intero è verificabile ma scomodo in dispute.
- Conservate config + DBC + ancore insieme al journal: tra tre anni la
  domanda sarà "con quale tariffa e quale decode fu misurata", e la risposta
  deve stare nell'archivio, non nella memoria di qualcuno.
