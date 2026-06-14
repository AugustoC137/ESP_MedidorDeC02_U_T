import json
import mysql.connector
import paho.mqtt.client as mqtt

# Configuração da conexão com o banco de dados MySQL
db = mysql.connector.connect(
    host="localhost",                             // Substitua pelo endereço do seu servidor de banco de dados, se não for local
    user="root",                                  // Substitua pelo nome de usuário do seu banco de dados
    password="Senha do server do banco de dados", // Substitua pela senha do seu banco de dados
    database="lab_monitor"                        // Substitua pelo nome do seu banco de dados
)
cursor = db.cursor()

# Função chamada quando uma mensagem MQTT é recebida
# Esta função decodifica o payload, extrai os dados e os insere no banco de dados
# Ela também lida com erros de decodificação JSON e outros erros de inserção
def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode("utf-8")
        data = json.loads(payload)

        device = data.get("device", "ESP32-Desconhecido")
        timestamp_ms = data.get("timestamp_ms")
        co2 = data.get("co2")
        temperatura = data.get("temperature")
        umidade = data.get("humidity")

        if co2 is None or temperatura is None or umidade is None:
            print("Mensagem incompleta recebida:", data)
            return

        sql = """
            INSERT INTO leituras (sensor, timestamp_ms, co2, temperatura, umidade)
            VALUES (%s, %s, %s, %s, %s)
        """
        val = (device, timestamp_ms, co2, temperatura, umidade)

        cursor.execute(sql, val)
        db.commit()

        print("Dados inseridos:", val)

    except json.JSONDecodeError:
        print("Erro: payload inválido.")
    except Exception as e:
        print("Erro ao inserir:", e)

# Função chamada quando o cliente MQTT se conecta ao broker
# Ela verifica o código de razão da conexão e se for bem-sucedida, inscreve-se no tópico "lab/sala1"
# Se a conexão falhar, imprime o código de razão da falha
def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print("Conectado ao broker MQTT com sucesso.")
        client.subscribe("lab/sala1")
    else:
        print("Falha na conexão MQTT. Código:", reason_code)

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message
client.connect("localhost", 1883, 60) # Substitua pelo endereço do seu broker MQTT, se não for local
client.loop_forever()