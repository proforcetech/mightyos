import QtQuick

// Turns mouse-wheel / touchpad deltas into whole navigation steps: one step
// per wheel notch (120 units of angleDelta); smaller touchpad deltas
// accumulate. Positive steps mean forward (down / right).
WheelHandler {
    id: stepper

    signal stepped(int steps)

    property real _accumulated: 0

    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    onWheel: (event) => {
        const dy = event.angleDelta.y
        const dx = event.angleDelta.x
        stepper._accumulated -= Math.abs(dy) >= Math.abs(dx) ? dy : dx
        const steps = Math.trunc(stepper._accumulated / 120)
        if (steps !== 0) {
            stepper._accumulated -= steps * 120
            stepper.stepped(steps)
        }
    }
}
