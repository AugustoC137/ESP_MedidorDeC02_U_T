#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SensirionI2cScd30.h>

// =====================
// WI-FI DE CASA
// =====================
const char* ssid = "augusto";          // Preencher com o nome do WI-FI
const char* password = "12345678";     // Senha do WI-FI

// =====================
// MQTT
// =====================
const char* mqtt_server = "192.168.15.7";  // Preencher com o ipv4 wifi do computador que está rodando o broker MQTT exemplo: 192.168.15.7
const int mqtt_port = 1883;
const char* mqtt_topic = "lab/sala1";
const char* device_id = "ESP32-SCD30-1";

// =====================
// SCD30
// =====================
#define SDA_PIN 21
#define SCL_PIN 22

SensirionI2cScd30 scd30;

WiFiClient espClient;
PubSubClient client(espClient);

// =====================
// ESTRUTURA E FILA (FreeRTOS)
// =====================
// Estrutura para empacotar os dados do sensor
struct DadosSensor {
  float co2;
  float temperature;
  float humidity;
  unsigned long timestamp;
};

// Handle da Fila de comunicação
QueueHandle_t filaSensores;

// =====================
// FUNÇÕES AUXILIARES DE REDE
// =====================
void setup_wifi() {
  Serial.print("Conectando ao WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    // Usando vTaskDelay no lugar de delay padrão para o RTOS
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.print(".");
  }

  Serial.println("\nWiFi conectado!");
  Serial.print("IP do ESP32: ");
  Serial.println(WiFi.localIP());
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Tentando conexao MQTT... ");
    String clientId = String(device_id) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client.connect(clientId.c_str())) {
      Serial.println("conectado!");
    } else {
      Serial.print("falhou, rc=");
      Serial.print(client.state());
      Serial.println(" tentando novamente em 5 segundos");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

// =====================
// INICIALIZA SCD30
// =====================
bool initSCD30() {
  Wire.begin(SDA_PIN, SCL_PIN);
  scd30.begin(Wire, SCD30_I2C_ADDR_61);
  scd30.stopPeriodicMeasurement();
  scd30.softReset();
  
  vTaskDelay(pdMS_TO_TICKS(2000));

  int16_t error = scd30.startPeriodicMeasurement(0);
  if (error != 0) {
    Serial.println("Erro ao iniciar o SCD30.");
    return false;
  }

  Serial.println("SCD30 inicializado com sucesso.");
  return true;
}

// =====================
// TAREFA 1: LEITURA DO SENSOR
// =====================
void taskSensor(void *pvParameters) {
  for (;;) {
    float co2 = 0;
    float temperature = 0;
    float humidity = 0;

    // A leitura bloqueante afeta apenas esta tarefa, o WiFi/MQTT continuam rodando na outra
    int16_t error = scd30.blockingReadMeasurementData(co2, temperature, humidity);

    if (error == 0 && co2 > 0 && humidity >= 0 && humidity <= 100) {
      // Prepara o pacote de dados
      DadosSensor dados;
      dados.co2 = co2;
      dados.temperature = temperature;
      dados.humidity = humidity;
      dados.timestamp = millis();

      // Envia os dados para a fila (timeout de 100ms se a fila estiver cheia)
      if (xQueueSend(filaSensores, &dados, pdMS_TO_TICKS(100)) != pdPASS) {
        Serial.println("Fila cheia, dados perdidos.");
      }
    } else {
      Serial.println("Erro na leitura do SCD30 ou leitura invalida.");
    }

    // Aguarda 5 segundos antes da proxima leitura
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// =====================
// TAREFA 2: REDE E MQTT
// =====================
void taskNetwork(void *pvParameters) {
  // Inicializa a rede de fato apenas na tarefa de rede
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);

  for (;;) {
    // Garante que o WiFi está conectado
    if (WiFi.status() != WL_CONNECTED) {
      setup_wifi();
    }

    // Garante que o MQTT está conectado
    if (!client.connected() && WiFi.status() == WL_CONNECTED) {
      reconnectMQTT();
    }

    // Mantém o MQTT vivo
    client.loop();

    // Tenta ler dados da fila (timeout de 500ms)
    DadosSensor dadosRecebidos;
    if (xQueueReceive(filaSensores, &dadosRecebidos, pdMS_TO_TICKS(500)) == pdPASS) {
      
      // Se recebeu os dados da fila, formata e publica
      char payload[256];
      snprintf(
        payload, sizeof(payload),
        "{\"device\":\"%s\",\"timestamp_ms\":%lu,\"co2\":%.2f,\"temperature\":%.2f,\"humidity\":%.2f}",
        device_id,
        dadosRecebidos.timestamp,
        dadosRecebidos.co2,
        dadosRecebidos.temperature,
        dadosRecebidos.humidity
      );

      Serial.print("Publicando: ");
      Serial.println(payload);
      client.publish(mqtt_topic, payload);
    }

    // Um pequeno delay para alimentar o Watchdog Timer (WDT) e ceder tempo de CPU
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  delay(1000); // Único delay normal permitido no boot

  Serial.println("Iniciando ESP32 + SCD30 + WiFi/MQTT (FreeRTOS)");

  // Tenta inicializar o SCD30, trava o ESP32 em caso de erro fatal
  if (!initSCD30()) {
    Serial.println("Erro critico: SCD30 nao inicializou.");
    while (true) {
      delay(1000);
    }
  }

  // Cria a fila capaz de armazenar até 10 leituras do sensor
  filaSensores = xQueueCreate(10, sizeof(DadosSensor));

  if (filaSensores == NULL) {
    Serial.println("Erro ao criar a fila do FreeRTOS.");
    while (true);
  }

  // Cria as Tarefas (Tasks)
  // Fixamos a rede no Core 1 (onde a stack do WiFi costuma rodar melhor no ESP32)
  xTaskCreatePinnedToCore(
    taskNetwork,    // Função que implementa a tarefa
    "NetworkTask",  // Nome da tarefa (para debug)
    8192,           // Tamanho da pilha (stack) em bytes (8KB)
    NULL,           // Parâmetros para a tarefa
    1,              // Prioridade (1 é padrão)
    NULL,           // Handle da tarefa
    1               // Core 1
  );

  // Fixamos a leitura do sensor no Core 0
  xTaskCreatePinnedToCore(
    taskSensor,     
    "SensorTask",   
    4096,           // Stack de 4KB (suficiente para ler I2C)
    NULL,           
    1,              // Prioridade
    NULL,           
    0               // Core 0
  );
}

// =====================
// LOOP MAIN
// =====================
void loop() {
  // Como estamos usando FreeRTOS, o loop principal de Arduino não precisa fazer nada.
  // Deletamos a tarefa padrão do loop para liberar memoria.
  vTaskDelete(NULL);
}