#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SensirionI2cScd30.h>
#include <stdarg.h>

// =====================================================
// CONFIGURAÇÕES - WI-FI DE CASA
// =====================================================

const char* ssid = "augusto";
const char* password = "12345678";

// =====================================================
// CONFIGURAÇÕES MQTT
// =====================================================

const char* mqtt_server = "10.19.24.155";
const int mqtt_port = 1883;
const char* mqtt_topic = "lab/sala1";
const char* device_id = "ESP32-SCD30-1";

// =====================================================
// CONFIGURAÇÕES SCD30
// =====================================================

#define SDA_PIN 21
#define SCL_PIN 22

SensirionI2cScd30 scd30;
WiFiClient espClient;
PubSubClient client(espClient);

// =====================================================
// DADOS DO SENSOR
// =====================================================

struct DadosSensor {
  float co2;
  float temperature;
  float humidity;
  unsigned long timestamp;
};

// Fila entre taskSensor e taskStateMachine
QueueHandle_t filaSensores;

// Mutexes
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t i2cMutex;

// =====================================================
// ESTADOS DA MÁQUINA
// =====================================================

enum EstadoSistema {
  INIT_COMPONENTES = 0,
  AGUARDANDO_WIFI,
  FALHA_WIFI,
  AGUARDANDO_MQTT,
  FALHA_MQTT,
  AGUARDANDO_DADOS,
  ENVIANDO_DADOS,
  FALHA_ENVIO,
  NUM_ESTADOS
};

// =====================================================
// EVENTOS DA MÁQUINA
// =====================================================

enum EventoSistema {
  EV_START = 0,
  EV_WIFI_OK,
  EV_WIFI_FAIL,
  EV_RETRY,
  EV_MQTT_OK,
  EV_MQTT_OK_COM_DADO_PENDENTE,
  EV_MQTT_FAIL,
  EV_DADOS_DISPONIVEIS,
  EV_SEM_DADOS,
  EV_ENVIO_OK,
  EV_ENVIO_FAIL,
  EV_WIFI_LOST,
  EV_MQTT_LOST,
  NUM_EVENTOS
};

// Matriz de transição
EstadoSistema matrizTransicao[NUM_ESTADOS][NUM_EVENTOS];

// Estado global da máquina
EstadoSistema estadoAtual = INIT_COMPONENTES;

// Dado pendente de envio
DadosSensor dadoPendente;
bool existeDadoPendente = false;

// =====================================================
// FUNÇÕES DE DEBUG
// =====================================================

const char* nomeEstado(EstadoSistema estado) {
  switch (estado) {
    case INIT_COMPONENTES: return "INIT_COMPONENTES";
    case AGUARDANDO_WIFI: return "AGUARDANDO_WIFI";
    case FALHA_WIFI: return "FALHA_WIFI";
    case AGUARDANDO_MQTT: return "AGUARDANDO_MQTT";
    case FALHA_MQTT: return "FALHA_MQTT";
    case AGUARDANDO_DADOS: return "AGUARDANDO_DADOS";
    case ENVIANDO_DADOS: return "ENVIANDO_DADOS";
    case FALHA_ENVIO: return "FALHA_ENVIO";
    default: return "ESTADO_DESCONHECIDO";
  }
}

const char* nomeEvento(EventoSistema evento) {
  switch (evento) {
    case EV_START: return "EV_START";
    case EV_WIFI_OK: return "EV_WIFI_OK";
    case EV_WIFI_FAIL: return "EV_WIFI_FAIL";
    case EV_RETRY: return "EV_RETRY";
    case EV_MQTT_OK: return "EV_MQTT_OK";
    case EV_MQTT_OK_COM_DADO_PENDENTE: return "EV_MQTT_OK_COM_DADO_PENDENTE";
    case EV_MQTT_FAIL: return "EV_MQTT_FAIL";
    case EV_DADOS_DISPONIVEIS: return "EV_DADOS_DISPONIVEIS";
    case EV_SEM_DADOS: return "EV_SEM_DADOS";
    case EV_ENVIO_OK: return "EV_ENVIO_OK";
    case EV_ENVIO_FAIL: return "EV_ENVIO_FAIL";
    case EV_WIFI_LOST: return "EV_WIFI_LOST";
    case EV_MQTT_LOST: return "EV_MQTT_LOST";
    default: return "EVENTO_DESCONHECIDO";
  }
}

void logPrintf(const char* format, ...) {
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    va_list args;
    va_start(args, format);
    Serial.vprintf(format, args);
    va_end(args);
    xSemaphoreGive(serialMutex);
  }
}

// =====================================================
// INICIALIZA MATRIZ DE TRANSIÇÃO
// =====================================================

void inicializarMatrizTransicao() {
  // Por padrão, qualquer evento não previsto mantém o estado atual.
  for (int estado = 0; estado < NUM_ESTADOS; estado++) {
    for (int evento = 0; evento < NUM_EVENTOS; evento++) {
      matrizTransicao[estado][evento] = (EstadoSistema)estado;
    }
  }

  matrizTransicao[INIT_COMPONENTES][EV_START] = AGUARDANDO_WIFI;

  matrizTransicao[AGUARDANDO_WIFI][EV_WIFI_OK] = AGUARDANDO_MQTT;
  matrizTransicao[AGUARDANDO_WIFI][EV_WIFI_FAIL] = FALHA_WIFI;

  matrizTransicao[FALHA_WIFI][EV_RETRY] = AGUARDANDO_WIFI;

  matrizTransicao[AGUARDANDO_MQTT][EV_MQTT_OK] = AGUARDANDO_DADOS;
  matrizTransicao[AGUARDANDO_MQTT][EV_MQTT_OK_COM_DADO_PENDENTE] = ENVIANDO_DADOS;
  matrizTransicao[AGUARDANDO_MQTT][EV_MQTT_FAIL] = FALHA_MQTT;
  matrizTransicao[AGUARDANDO_MQTT][EV_WIFI_LOST] = AGUARDANDO_WIFI;

  matrizTransicao[FALHA_MQTT][EV_RETRY] = AGUARDANDO_MQTT;
  matrizTransicao[FALHA_MQTT][EV_WIFI_LOST] = AGUARDANDO_WIFI;

  matrizTransicao[AGUARDANDO_DADOS][EV_DADOS_DISPONIVEIS] = ENVIANDO_DADOS;
  matrizTransicao[AGUARDANDO_DADOS][EV_WIFI_LOST] = AGUARDANDO_WIFI;
  matrizTransicao[AGUARDANDO_DADOS][EV_MQTT_LOST] = AGUARDANDO_MQTT;

  matrizTransicao[ENVIANDO_DADOS][EV_ENVIO_OK] = AGUARDANDO_DADOS;
  matrizTransicao[ENVIANDO_DADOS][EV_ENVIO_FAIL] = FALHA_ENVIO;
  matrizTransicao[ENVIANDO_DADOS][EV_WIFI_LOST] = AGUARDANDO_WIFI;
  matrizTransicao[ENVIANDO_DADOS][EV_MQTT_LOST] = AGUARDANDO_MQTT;

  matrizTransicao[FALHA_ENVIO][EV_RETRY] = ENVIANDO_DADOS;
  matrizTransicao[FALHA_ENVIO][EV_WIFI_LOST] = AGUARDANDO_WIFI;
  matrizTransicao[FALHA_ENVIO][EV_MQTT_LOST] = AGUARDANDO_MQTT;
}

void aplicarTransicao(EventoSistema evento) {
  EstadoSistema novoEstado = matrizTransicao[estadoAtual][evento];

  if (novoEstado != estadoAtual) {
    logPrintf(
      "[FSM] %s --(%s)--> %s\n",
      nomeEstado(estadoAtual),
      nomeEvento(evento),
      nomeEstado(novoEstado)
    );
  }

  estadoAtual = novoEstado;
}

// =====================================================
// WI-FI
// =====================================================

bool tentarConectarWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  logPrintf("[WiFi] Conectando em: %s\n", ssid);

  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > timeoutMs) {
      logPrintf("[WiFi] Timeout de conexao.\n");
      return false;
    }

    logPrintf(".");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  logPrintf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// =====================================================
// MQTT
// =====================================================

bool tentarConectarMQTT(uint32_t timeoutMs) {
  if (client.connected()) {
    return true;
  }

  client.setServer(mqtt_server, mqtt_port);

  unsigned long inicio = millis();

  while (!client.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }

    if (millis() - inicio > timeoutMs) {
      logPrintf("[MQTT] Timeout de conexao.\n");
      return false;
    }

    String clientId = String(device_id) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    logPrintf("[MQTT] Tentando conectar como %s...\n", clientId.c_str());

    if (client.connect(clientId.c_str())) {
      logPrintf("[MQTT] Conectado.\n");
      return true;
    }

    logPrintf("[MQTT] Falha rc=%d. Tentando novamente...\n", client.state());
    vTaskDelay(pdMS_TO_TICKS(3000));
  }

  return true;
}

bool publicarMQTT(DadosSensor dados) {
  char payload[256];

  snprintf(
    payload,
    sizeof(payload),
    "{\"device\":\"%s\",\"timestamp_ms\":%lu,\"co2\":%.2f,\"temperature\":%.2f,\"humidity\":%.2f}",
    device_id,
    dados.timestamp,
    dados.co2,
    dados.temperature,
    dados.humidity
  );

  logPrintf("[MQTT] Publicando: %s\n", payload);

  bool enviado = client.publish(mqtt_topic, payload);

  if (enviado) {
    logPrintf("[MQTT] Envio concluido.\n");
  } else {
    logPrintf("[MQTT] Falha no envio.\n");
  }

  return enviado;
}

// =====================================================
// SCD30
// =====================================================

bool initSCD30() {
  Wire.begin(SDA_PIN, SCL_PIN);

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    scd30.begin(Wire, SCD30_I2C_ADDR_61);

    scd30.stopPeriodicMeasurement();
    scd30.softReset();

    xSemaphoreGive(i2cMutex);
  } else {
    Serial.println("Erro: I2C ocupado durante init.");
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(2000));

  int16_t error;

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    error = scd30.startPeriodicMeasurement(0);
    xSemaphoreGive(i2cMutex);
  } else {
    Serial.println("Erro: I2C ocupado ao iniciar medicao.");
    return false;
  }

  if (error != 0) {
    Serial.println("Erro ao iniciar o SCD30.");
    return false;
  }

  Serial.println("SCD30 inicializado com sucesso.");
  return true;
}

// =====================================================
// TASK 1 - SENSOR
// =====================================================
//
// Esta task é independente da rede.
// Ela apenas lê o sensor e coloca os dados na fila.
//
// Prioridade: 2
// Justificativa: leitura ambiental é a função principal do sistema.

void taskSensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t periodoLeitura = pdMS_TO_TICKS(5000);

  for (;;) {
    float co2 = 0;
    float temperature = 0;
    float humidity = 0;

    int16_t error = -1;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      error = scd30.blockingReadMeasurementData(co2, temperature, humidity);
      xSemaphoreGive(i2cMutex);
    } else {
      logPrintf("[Sensor] I2C ocupado. Leitura ignorada.\n");
    }

    if (error == 0) {
      bool valido =
        co2 > 0 &&
        co2 < 10000 &&
        temperature > -20 &&
        temperature < 80 &&
        humidity >= 0 &&
        humidity <= 100;

      if (valido) {
        DadosSensor dados;
        dados.co2 = co2;
        dados.temperature = temperature;
        dados.humidity = humidity;
        dados.timestamp = millis();

        if (xQueueSend(filaSensores, &dados, pdMS_TO_TICKS(100)) != pdPASS) {
          // Se a fila estiver cheia, descarta o mais antigo e coloca o novo.
          DadosSensor descartado;
          xQueueReceive(filaSensores, &descartado, 0);
          xQueueSend(filaSensores, &dados, 0);

          logPrintf("[Sensor] Fila cheia. Dado antigo descartado.\n");
        }

        logPrintf(
          "[Sensor] CO2=%.2f ppm | T=%.2f C | UR=%.2f %%\n",
          co2,
          temperature,
          humidity
        );
      } else {
        logPrintf("[Sensor] Leitura fora da faixa esperada.\n");
      }
    } else {
      logPrintf("[Sensor] Erro na leitura do SCD30.\n");
    }

    vTaskDelayUntil(&xLastWakeTime, periodoLeitura);
  }
}

// =====================================================
// TASK 2 - MÁQUINA DE ESTADOS
// =====================================================
//
// Esta task implementa explicitamente o fluxo:
// WiFi -> MQTT -> aguarda dados -> envia dados -> trata falhas.
//
// Prioridade: 1
// Justificativa: rede pode ser lenta e falhar. Ela não deve travar o sensor.

void taskStateMachine(void *pvParameters) {
  EventoSistema evento = EV_START;

  aplicarTransicao(evento);

  for (;;) {
    switch (estadoAtual) {
      case AGUARDANDO_WIFI: {
        bool ok = tentarConectarWiFi(20000);

        if (ok) {
          evento = EV_WIFI_OK;
        } else {
          evento = EV_WIFI_FAIL;
        }

        aplicarTransicao(evento);
        break;
      }

      case FALHA_WIFI: {
        logPrintf("[FSM] Falha WiFi. Nova tentativa em 5 segundos.\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        aplicarTransicao(EV_RETRY);
        break;
      }

      case AGUARDANDO_MQTT: {
        if (WiFi.status() != WL_CONNECTED) {
          aplicarTransicao(EV_WIFI_LOST);
          break;
        }

        bool ok = tentarConectarMQTT(15000);

        if (ok) {
          if (existeDadoPendente) {
            aplicarTransicao(EV_MQTT_OK_COM_DADO_PENDENTE);
          } else {
            aplicarTransicao(EV_MQTT_OK);
          }
        } else {
          aplicarTransicao(EV_MQTT_FAIL);
        }

        break;
      }

      case FALHA_MQTT: {
        logPrintf("[FSM] Falha MQTT. Nova tentativa em 5 segundos.\n");

        if (WiFi.status() != WL_CONNECTED) {
          aplicarTransicao(EV_WIFI_LOST);
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        aplicarTransicao(EV_RETRY);
        break;
      }

      case AGUARDANDO_DADOS: {
        if (WiFi.status() != WL_CONNECTED) {
          aplicarTransicao(EV_WIFI_LOST);
          break;
        }

        if (!client.connected()) {
          aplicarTransicao(EV_MQTT_LOST);
          break;
        }

        client.loop();

        DadosSensor dadosRecebidos;

        if (xQueueReceive(filaSensores, &dadosRecebidos, pdMS_TO_TICKS(500)) == pdPASS) {
          dadoPendente = dadosRecebidos;
          existeDadoPendente = true;
          aplicarTransicao(EV_DADOS_DISPONIVEIS);
        } else {
          aplicarTransicao(EV_SEM_DADOS);
        }

        break;
      }

      case ENVIANDO_DADOS: {
        if (WiFi.status() != WL_CONNECTED) {
          aplicarTransicao(EV_WIFI_LOST);
          break;
        }

        if (!client.connected()) {
          aplicarTransicao(EV_MQTT_LOST);
          break;
        }

        client.loop();

        if (!existeDadoPendente) {
          aplicarTransicao(EV_ENVIO_OK);
          break;
        }

        bool enviado = publicarMQTT(dadoPendente);

        if (enviado) {
          existeDadoPendente = false;
          aplicarTransicao(EV_ENVIO_OK);
        } else {
          aplicarTransicao(EV_ENVIO_FAIL);
        }

        break;
      }

      case FALHA_ENVIO: {
        logPrintf("[FSM] Falha no envio. Tentando reenviar em 3 segundos.\n");

        if (WiFi.status() != WL_CONNECTED) {
          aplicarTransicao(EV_WIFI_LOST);
          break;
        }

        if (!client.connected()) {
          aplicarTransicao(EV_MQTT_LOST);
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
        aplicarTransicao(EV_RETRY);
        break;
      }

      case INIT_COMPONENTES:
      default: {
        aplicarTransicao(EV_START);
        break;
      }
    }

    // Cede CPU para outras tasks e evita watchdog.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  vTaskDelay(pdMS_TO_TICKS(1000));

  Serial.println("\nIniciando ESP32 + SCD30 + MQTT + FSM + FreeRTOS");

  serialMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();

  if (serialMutex == NULL || i2cMutex == NULL) {
    Serial.println("Erro ao criar mutexes.");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  inicializarMatrizTransicao();

  if (!initSCD30()) {
    Serial.println("Erro critico: SCD30 nao inicializou.");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  filaSensores = xQueueCreate(10, sizeof(DadosSensor));

  if (filaSensores == NULL) {
    Serial.println("Erro ao criar fila de sensores.");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  xTaskCreatePinnedToCore(
    taskSensor,
    "SensorTask",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    taskStateMachine,
    "StateMachineTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println("Tasks criadas com sucesso.");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  vTaskDelete(NULL);
}