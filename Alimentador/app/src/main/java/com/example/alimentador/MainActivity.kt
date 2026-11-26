package com.example.alimentador

import android.app.TimePickerDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.firebase.database.*
import java.util.Calendar

data class Rutina(
    val id: String = "",
    val hora: String = "08:00",
    val gramos: Int = 0,
    val activa: Boolean = true
)

data class SesionRutina(
    val origen: String = "",
    val hora_dispensado: String = "",
    val gramos_servidos: Int = 0,
    val hora_fue_a_comer: String = "Pendiente",
    val gramos_comidos: Int = 0
)

data class LogEvento(
    val tipo: String = "",      // "dispensacion" o "acceso_nfc"
    val detalle: String = "",   // ej. "50g servidos" o "Tapa abierta - Gato detectado"
    val timestamp: String = ""  // ej. "2026-11-19 18:10:00"
)

class MainActivity : AppCompatActivity() {

    private lateinit var database: DatabaseReference
    private lateinit var tvEstadoWifi: TextView
    private lateinit var tvPesoPlato: TextView
    private lateinit var tvEstadoTolva: TextView
    private lateinit var tvSinRutinas: TextView
    private lateinit var llContenedorRutinas: LinearLayout
    private lateinit var tvSinHistorial: TextView
    private lateinit var llContenedorHistorial: LinearLayout
    private lateinit var tvSinLogs: TextView
    private lateinit var llContenedorLogs: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        database = FirebaseDatabase.getInstance().reference

        tvEstadoWifi = findViewById(R.id.tvEstadoWifi)
        tvPesoPlato = findViewById(R.id.tvPesoPlato)
        tvEstadoTolva = findViewById(R.id.tvEstadoTolva)
        tvSinRutinas = findViewById(R.id.tvSinRutinas)
        llContenedorRutinas = findViewById(R.id.llContenedorRutinas)
        tvSinHistorial = findViewById(R.id.tvSinHistorial)
        llContenedorHistorial = findViewById(R.id.llContenedorHistorial)
        tvSinLogs = findViewById(R.id.tvSinLogs)
        llContenedorLogs = findViewById(R.id.llContenedorLogs)

        val btnDispensarAhora: Button = findViewById(R.id.btnDispensarAhora)
        val btnNuevaRutina: Button = findViewById(R.id.btnNuevaRutina)

        escucharEstadoDispositivo()
        escucharRutinas()
        escucharHistorialRutinas()
        escucharLogs()

        btnDispensarAhora.setOnClickListener {
            val inputGramos = EditText(this).apply {
                inputType = InputType.TYPE_CLASS_NUMBER
                hint = "Ej: 50"
            }

            AlertDialog.Builder(this)
                .setTitle("Dispensar comida")
                .setMessage("¿Cuántos gramos totales deseas en el plato?")
                .setView(inputGramos)
                .setPositiveButton("Dispensar") { _, _ ->
                    val gramos = inputGramos.text.toString().toIntOrNull()
                    if (gramos != null && gramos > 0) {
                        val updates = mapOf<String, Any>(
                            "gramos_manual" to gramos,
                            "dispensar_ahora" to true
                        )
                        database.child("dispositivo").updateChildren(updates)
                        Toast.makeText(this, "Orden enviada: completar ${gramos}g", Toast.LENGTH_SHORT).show()
                    } else {
                        Toast.makeText(this, "Ingresa una cantidad válida", Toast.LENGTH_SHORT).show()
                    }
                }
                .setNegativeButton("Cancelar", null)
                .show()
        }

        btnNuevaRutina.setOnClickListener {
            mostrarDialogoRutina(null)
        }
    }

    private fun escucharEstadoDispositivo() {
        database.child("dispositivo").addValueEventListener(object : ValueEventListener {
            override fun onDataChange(snapshot: DataSnapshot) {
                val wifiOnline = snapshot.child("estado_wifi").getValue(Boolean::class.java) ?: false
                val pesoActual = snapshot.child("peso_actual_plato").getValue(Int::class.java) ?: 0
                val tolvaBaja = snapshot.child("tolva_baja").getValue(Boolean::class.java) ?: false

                tvEstadoWifi.text = if (wifiOnline) "Estado: En línea (Wi-Fi)" else "Estado: Desconectado"
                tvPesoPlato.text = "Peso actual en plato: ${pesoActual}g"
                tvEstadoTolva.text = if (tolvaBaja) "Tolva: ¡Nivel bajo! Reabastecer" else "Tolva: Nivel normal"
            }

            override fun onCancelled(error: DatabaseError) {}
        })
    }

    private fun escucharRutinas() {
        database.child("rutinas").addValueEventListener(object : ValueEventListener {
            override fun onDataChange(snapshot: DataSnapshot) {
                llContenedorRutinas.removeAllViews()

                if (!snapshot.exists()) {
                    tvSinRutinas.visibility = View.VISIBLE
                    return
                }

                tvSinRutinas.visibility = View.GONE
                val inflater = LayoutInflater.from(this@MainActivity)

                for (dato in snapshot.children) {
                    val rutina = dato.getValue(Rutina::class.java) ?: continue
                    val idReal = dato.key ?: continue
                    val rutinaConId = rutina.copy(id = idReal)

                    val fila = inflater.inflate(R.layout.item_rutina, llContenedorRutinas, false)
                    val tvInfo: TextView = fila.findViewById(R.id.tvInfoRutina)
                    val swActiva: Switch = fila.findViewById(R.id.swActiva)
                    val btnEditar: Button = fila.findViewById(R.id.btnEditar)
                    val btnEliminar: Button = fila.findViewById(R.id.btnEliminar)

                    tvInfo.text = "${rutinaConId.hora}  —  ${rutinaConId.gramos}g"
                    swActiva.isChecked = rutinaConId.activa

                    swActiva.setOnCheckedChangeListener { _, isChecked ->
                        database.child("rutinas").child(idReal).child("activa").setValue(isChecked)
                    }

                    btnEditar.setOnClickListener {
                        mostrarDialogoRutina(rutinaConId)
                    }

                    btnEliminar.setOnClickListener {
                        AlertDialog.Builder(this@MainActivity)
                            .setTitle("Eliminar rutina")
                            .setMessage("¿Seguro que deseas eliminar la comida de las ${rutinaConId.hora}?")
                            .setPositiveButton("Eliminar") { _, _ ->
                                database.child("rutinas").child(idReal).removeValue()
                                Toast.makeText(this@MainActivity, "Rutina eliminada", Toast.LENGTH_SHORT).show()
                            }
                            .setNegativeButton("Cancelar", null)
                            .show()
                    }

                    llContenedorRutinas.addView(fila)
                }
            }

            override fun onCancelled(error: DatabaseError) {}
        })
    }

    private fun escucharHistorialRutinas() {
        database.child("historial_rutinas").limitToLast(15).addValueEventListener(object : ValueEventListener {
            override fun onDataChange(snapshot: DataSnapshot) {
                llContenedorHistorial.removeAllViews()

                if (!snapshot.exists()) {
                    tvSinHistorial.visibility = View.VISIBLE
                    return
                }

                tvSinHistorial.visibility = View.GONE

                val listaSesiones = mutableListOf<SesionRutina>()
                for (dato in snapshot.children) {
                    val sesion = dato.getValue(SesionRutina::class.java)
                    if (sesion != null) listaSesiones.add(sesion)
                }
                listaSesiones.reverse() // Más reciente arriba

                for (sesion in listaSesiones) {
                    val tarjeta = LinearLayout(this@MainActivity).apply {
                        orientation = LinearLayout.VERTICAL
                        setPadding(32, 24, 32, 24)
                        background = GradientDrawable().apply {
                            setColor(Color.parseColor("#F5F5F5"))
                            cornerRadius = 16f
                            setStroke(2, Color.parseColor("#E0E0E0"))
                        }
                        layoutParams = LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT
                        ).apply {
                            setMargins(0, 0, 0, 16)
                        }
                    }

                    val tvTitulo = TextView(this@MainActivity).apply {
                        text = sesion.origen
                        textSize = 16f
                        setTypeface(null, Typeface.BOLD)
                        setTextColor(Color.parseColor("#1565C0"))
                    }

                    val tvDispensado = TextView(this@MainActivity).apply {
                        text = "Dispensado: ${sesion.hora_dispensado} (${sesion.gramos_servidos}g servidos)"
                        textSize = 14f
                    }

                    val tvHoraComida = TextView(this@MainActivity).apply {
                        text = "Fue a comer: ${sesion.hora_fue_a_comer}"
                        textSize = 14f
                    }

                    val tvComido = TextView(this@MainActivity).apply {
                        text = "Comió: ${sesion.gramos_comidos}g"
                        textSize = 14f
                        setTypeface(null, Typeface.BOLD)
                        setTextColor(
                            if (sesion.gramos_comidos > 0) Color.parseColor("#2E7D32")
                            else Color.parseColor("#757575")
                        )
                    }

                    tarjeta.addView(tvTitulo)
                    tarjeta.addView(tvDispensado)
                    tarjeta.addView(tvHoraComida)
                    tarjeta.addView(tvComido)

                    llContenedorHistorial.addView(tarjeta)
                }
            }

            override fun onCancelled(error: DatabaseError) {}
        })
    }

    private fun escucharLogs() {
        database.child("logs").limitToLast(15).addValueEventListener(object : ValueEventListener {
            override fun onDataChange(snapshot: DataSnapshot) {
                llContenedorLogs.removeAllViews()

                if (!snapshot.exists()) {
                    tvSinLogs.visibility = View.VISIBLE
                    return
                }

                tvSinLogs.visibility = View.GONE
                val inflater = LayoutInflater.from(this@MainActivity)

                val listaLogs = mutableListOf<LogEvento>()
                for (dato in snapshot.children) {
                    val log = dato.getValue(LogEvento::class.java)
                    if (log != null) listaLogs.add(log)
                }
                listaLogs.reverse()

                for (log in listaLogs) {
                    val fila = inflater.inflate(R.layout.item_log, llContenedorLogs, false)
                    val tvTipo: TextView = fila.findViewById(R.id.tvLogTipo)
                    val tvDetalle: TextView = fila.findViewById(R.id.tvLogDetalle)
                    val tvFecha: TextView = fila.findViewById(R.id.tvLogFecha)

                    if (log.tipo == "acceso_nfc") {
                        tvTipo.text = "Acceso NFC (Gato detectado)"
                        tvTipo.setTextColor(Color.parseColor("#2E7D32"))
                    } else {
                        tvTipo.text = "Dispensación de alimento"
                        tvTipo.setTextColor(Color.parseColor("#1565C0"))
                    }

                    tvDetalle.text = log.detalle
                    tvFecha.text = log.timestamp

                    llContenedorLogs.addView(fila)
                }
            }

            override fun onCancelled(error: DatabaseError) {}
        })
    }

    private fun mostrarDialogoRutina(rutinaExistente: Rutina?) {
        val cal = Calendar.getInstance()
        var horaInicial = cal.get(Calendar.HOUR_OF_DAY)
        var minutoInicial = cal.get(Calendar.MINUTE)

        if (rutinaExistente != null) {
            val partes = rutinaExistente.hora.split(":")
            if (partes.size == 2) {
                horaInicial = partes[0].toIntOrNull() ?: horaInicial
                minutoInicial = partes[1].toIntOrNull() ?: minutoInicial
            }
        }

        TimePickerDialog(this, { _, hora, minuto ->
            val horaFormateada = String.format("%02d:%02d", hora, minuto)

            val inputGramos = EditText(this).apply {
                hint = "Ej. 50"
                inputType = InputType.TYPE_CLASS_NUMBER
                if (rutinaExistente != null) {
                    setText(rutinaExistente.gramos.toString())
                }
            }

            val titulo = if (rutinaExistente == null) "Nueva ración ($horaFormateada)" else "Editar ración ($horaFormateada)"

            AlertDialog.Builder(this)
                .setTitle(titulo)
                .setView(inputGramos)
                .setPositiveButton("Guardar") { _, _ ->
                    val gramos = inputGramos.text.toString().toIntOrNull() ?: 0
                    if (gramos > 0) {
                        if (rutinaExistente == null) {
                            val ref = database.child("rutinas").push()
                            val nueva = Rutina(ref.key ?: "", horaFormateada, gramos, true)
                            ref.setValue(nueva)
                            Toast.makeText(this, "Rutina creada", Toast.LENGTH_SHORT).show()
                        } else {
                            val actualizada = rutinaExistente.copy(hora = horaFormateada, gramos = gramos)
                            database.child("rutinas").child(rutinaExistente.id).setValue(actualizada)
                            Toast.makeText(this, "Rutina actualizada", Toast.LENGTH_SHORT).show()
                        }
                    } else {
                        Toast.makeText(this, "Ingresa una cantidad válida", Toast.LENGTH_SHORT).show()
                    }
                }
                .setNegativeButton("Cancelar", null)
                .show()

        }, horaInicial, minutoInicial, true).show()
    }
}