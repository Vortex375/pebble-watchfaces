// Clay settings page. Select values are sent as strings ("0".."5").
var metric = function (key, label, def) {
  return {
    type: 'select',
    messageKey: key,
    label: label,
    defaultValue: def,
    options: [
      { label: 'Nothing', value: '0' },
      { label: 'Weather', value: '1' },
      { label: 'Date', value: '2' },
      { label: 'Steps', value: '3' },
      { label: 'Heart rate', value: '4' },
      { label: 'Battery', value: '5' }
    ]
  };
};

module.exports = [
  { type: 'heading', defaultValue: 'Orbit' },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Time' },
      { type: 'toggle', messageKey: 'Ring12h', label: '12-hour hour ring', defaultValue: false,
        description: 'Off: the ring shows the whole day. On: it shows 6 hours either side of now.' },
      { type: 'toggle', messageKey: 'Digital12h', label: '12-hour digital time', defaultValue: false }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Metrics' },
      metric('Center', 'Centre', '3'),
      metric('CornerTL', 'Top left', '2'),
      metric('CornerTR', 'Top right', '1'),
      metric('CornerBL', 'Bottom left', '4'),
      metric('CornerBR', 'Bottom right', '5')
    ]
  },
  { type: 'submit', defaultValue: 'Save' }
];
